/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringConcatenator.h"

#include <boost/optional.hpp>
#include <unordered_map>
#include <utility>

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "Resolver.h"
#include "Walkers.h"

/* This pass finds <clinit> methods with lots of string concatenations of
 * compile-time-known strings and concatenates those strings. This way, at
 * runtime, we only need to load the string.
 *
 * Here's an example <clinit> method that this method will optimize:
 *
 * public static final String PREFIX = "foo";
 * public static final String CONCATENATED = PREFIX + "bar";
 *
 * The output code should be:
 *
 *   public static final PREFIX = "foo";
 *   public static final CONCATENATED = "foobar";
 *
 * This should be done after FinalInline pass to make sure input strings are
 * resolved.
 *
 * TODO: someday, this should probably be subsumed by a more general purpose
 * optimization, possible StringSimplificationPass.
 */

namespace {
using Register = uint32_t;
using StrBuilderId = uint32_t;

class RegMap {
  using RegStrMap = std::unordered_map<Register, std::string>;
  using RegBuilderMap = std::unordered_map<Register, StrBuilderId>;
  RegStrMap m_strings;
  RegBuilderMap m_builders;

 public:
  void put_string(Register r, const std::string& s) {
    m_builders.erase(r);
    m_strings[r] = s;
  }

  void put_builder(Register r, StrBuilderId b) {
    m_strings.erase(r);
    m_builders[r] = b;
  }

  boost::optional<std::string> find_string(Register r) {
    const auto& search = m_strings.find(r);
    return (search == m_strings.end())
               ? boost::none
               : boost::optional<std::string>(search->second);
  }

  boost::optional<StrBuilderId> find_builder(Register r) {
    const auto& search = m_builders.find(r);
    return (search == m_builders.end())
               ? boost::none
               : boost::optional<StrBuilderId>(search->second);
  }
};

struct Stats {
  uint32_t insns_removed{0};
  uint32_t methods_rewritten{0};
  uint32_t string_fields_resolved{0};

  void operator+=(const Stats& stats) {
    insns_removed += stats.insns_removed;
    methods_rewritten += stats.methods_rewritten;
    string_fields_resolved += stats.string_fields_resolved;
  }

  void report(PassManager& mgr) {
    mgr.set_metric("insns_removed", insns_removed);
    mgr.set_metric("methods_rewritten", methods_rewritten);
    mgr.set_metric("string_fields_resolved", string_fields_resolved);
    TRACE(
        STR_CAT, 1,
        "insns removed: %d, methods rewritten %d, string fields resolved %d\n",
        insns_removed, methods_rewritten, string_fields_resolved);
  }
};

class Concatenator {

  using BuilderStrMap = std::unordered_map<StrBuilderId, std::string>;
  using FieldMap = std::unordered_map<DexFieldRef*, std::string>;
  using FieldOrder = std::vector<std::pair<DexFieldRef*, MethodItemEntry*>>;

  const Register RESULT_REGISTER = std::numeric_limits<Register>::max() - 1;
  const DexType* m_string_builder;
  const DexType* m_string;
  const DexMethodRef* m_init_void;
  const DexMethodRef* m_init_string;
  const DexMethodRef* m_append;
  const DexMethodRef* m_to_string;

  /* match this (and similar) patterns:
   *
   *  new-instance v1, Ljava/lang/StringBuilder;
   *  const-string v0, "fb"
   *  invoke-direct {v1, v0}, Ljava/lang/StringBuilder;.<init>
   *  const-string v0, "://"
   *  invoke-virtual {v1, v0}, Ljava/lang/StringBuilder;.append
   *  move-result-object v0
   *  invoke-virtual {v0}, Ljava/lang/StringBuilder;.toString
   *  move-result-object v0
   *  sput-object v0, LFoo;.PREFIX:Ljava/lang/String;
   *
   *  new-instance v1, Ljava/lang/StringBuilder;
   *  invoke-direct {v1}, Ljava/lang/StringBuilder;.<init>:()V
   *  sget-object v0, LFoo;.PREFIX:Ljava/lang/String;
   *  invoke-virtual {v1, v0}, Ljava/lang/StringBuilder;.append
   *  move-result-object v1
   *  const-string v0, "bar"
   *  move-result-object v0
   *  invoke-virtual {v0}, Ljava/lang/StringBuilder;.toString
   *  move-result-object v0
   *  sput-object v0, LFoo;.CONCATENATED:Ljava/lang/String;
   *
   * And fill `fields_ptr` with the final values of these string fields
   */
  bool analyze(cfg::Block* block,
               const DexType* this_type,
               FieldMap* fields_ptr,
               FieldOrder* field_order) {
    RegMap registers;
    BuilderStrMap builder_str;
    FieldMap& fields = *fields_ptr;
    always_assert_log(fields.empty() && field_order->empty(),
                      "should start with fresh field maps");

    bool has_string_builder = false;
    bool has_to_string = false;
    MethodItemEntry* last_pos = nullptr;
    for (auto& mie : *block) {
      if (mie.type == MFLOW_POSITION) {
        last_pos = &mie;
      }
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }

      auto insn = mie.insn;
      auto op = insn->opcode();

      // The bottom of this switch statement has a `return false`. This way, any
      // instructions that we don't expect force us to safely abort the
      // analysis. A successful analysis of an instruction should immediately
      // be followed by a `continue` statement.
      //
      // TODO: allow static initializers that have other unrelated code
      switch (op) {
      case OPCODE_MOVE_RESULT_OBJECT:
      case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
        const auto& str_search = registers.find_string(RESULT_REGISTER);
        const auto& builder_search = registers.find_builder(RESULT_REGISTER);
        if (str_search != boost::none) {
          always_assert(builder_search == boost::none);
          registers.put_string(insn->dest(), *str_search);
          continue;
        } else if (builder_search != boost::none) {
          always_assert(str_search == boost::none);
          registers.put_builder(insn->dest(), *builder_search);
          continue;
        }
        break;
      }
      case OPCODE_CONST_STRING:
        registers.put_string(RESULT_REGISTER, insn->get_string()->str());
        continue;
      case OPCODE_NEW_INSTANCE:
        if (insn->get_type() == m_string_builder) {
          StrBuilderId new_id = builder_str.size();
          builder_str[new_id] = "";
          registers.put_builder(RESULT_REGISTER, new_id);

          has_string_builder = true;
          continue;
        }
        break;
      case OPCODE_SGET_OBJECT: {
        const auto& field_search = fields.find(insn->get_field());
        if (field_search != fields.end()) {
          registers.put_string(RESULT_REGISTER, field_search->second);
          continue;
        }
        break;
      }
      case OPCODE_SPUT_OBJECT: {
        const auto& field_ref = insn->get_field();
        if (field_ref->get_type() == m_string &&
            field_ref->get_class() == this_type && last_pos != nullptr) {
          const auto& field_def = resolve_field(field_ref, FieldSearch::Static);
          if (field_def != nullptr && is_final(field_def)) {
            const auto& str_search = registers.find_string(insn->src(0));
            if (str_search != boost::none) {
              fields[field_ref] = *str_search;
              field_order->emplace_back(field_ref, last_pos);
              continue;
            }
          }
        }
        break;
      }
      case OPCODE_INVOKE_VIRTUAL:
      case OPCODE_INVOKE_DIRECT: {
        auto method = insn->get_method();
        if (method == m_init_void) {
          continue;
        } else if (method == m_init_string) {
          const auto& str_search = registers.find_string(insn->src(1));
          if (str_search != boost::none) {
            StrBuilderId new_id = builder_str.size();
            builder_str[new_id] = *str_search;
            registers.put_builder(insn->src(0), new_id);
            continue;
          }
        } else if (method == m_append) {
          auto builder_search = registers.find_builder(insn->src(0));
          const auto& string_search = registers.find_string(insn->src(1));
          if (builder_search != boost::none && string_search != boost::none) {
            builder_str[*builder_search] += *string_search;
            registers.put_builder(RESULT_REGISTER, *builder_search);
            continue;
          }
        } else if (method == m_to_string) {
          const auto& builder_search = registers.find_builder(insn->src(0));
          if (builder_search != boost::none) {
            registers.put_string(RESULT_REGISTER, builder_str[*builder_search]);

            has_to_string = true;
            continue;
          }
        }
        break;
      }
      case OPCODE_RETURN_VOID:
        continue;
      default:
        break;
      }
      // reject any method with any other instruction
      return false;
    }
    // filter out methods that had a few instructions we analyzed but didn't
    // actually have a field loaded by a concatenated string
    return has_string_builder && has_to_string && !fields.empty();
  }

  /* remove everything except the source code positions, then write new code
   * that fills in fields with their known values at the end of the clinit.
   */
  void rewrite(IRList* entries,
               const FieldMap& fields,
               const FieldOrder& field_order) {
    std::unordered_set<MethodItemEntry*> positions;
    for (const auto& pair : field_order) {
      positions.insert(pair.second);
    }

    for (auto it = entries->begin(); it != entries->end();) {
      if (positions.count(&*it) == 0) {
        it = entries->erase(it);
      } else {
        ++it;
      }
    }

    // FIXME: if two field loads share the same line, we'll reverse their order?
    for (const auto& pair : field_order) {
      // const-string "foobar"
      // move-result-object v0
      // sput v0 Lcom/Cls;.field;
      DexFieldRef* field = pair.first;
      IRList::iterator position = entries->iterator_to(*pair.second);

      IRInstruction* string_load = new IRInstruction(OPCODE_CONST_STRING);
      string_load->set_string(DexString::make_string(fields.at(field)));

      IRInstruction* move_res =
          new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
      move_res->set_dest(0);

      IRInstruction* sput = new IRInstruction(OPCODE_SPUT_OBJECT);
      sput->set_src(0, 0);
      sput->set_field(field);

      // insert in reverse order because we're inserting after a position that
      // doesn't move
      entries->insert_after(position, sput);
      entries->insert_after(position, move_res);
      entries->insert_after(position, string_load);
    }
    entries->push_back(
        *new MethodItemEntry(new IRInstruction(OPCODE_RETURN_VOID)));
  }

 public:
  Concatenator() {
    m_string_builder = DexType::get_type("Ljava/lang/StringBuilder;");
    always_assert(m_string_builder != nullptr);
    m_string = DexType::get_type("Ljava/lang/String;");
    always_assert(m_string != nullptr);
    m_init_void = DexMethod::get_method("Ljava/lang/StringBuilder;.<init>:()V");
    always_assert(m_init_void != nullptr);
    m_init_string = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V");
    always_assert(m_init_string != nullptr);
    m_append = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
        "StringBuilder;");
    always_assert(m_append != nullptr);
    m_to_string = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;");
    always_assert(m_to_string != nullptr);
  }

  Stats run(cfg::ControlFlowGraph* cfg, DexMethod* method) {
    Stats stats;
    const auto& blocks = cfg->blocks();
    if (blocks.size() != 1) {
      // We're interested in the massive initialization functions for classes
      // with compile-time-known strings. These methods don't usually have more
      // than one block.
      return stats;
    }
    cfg::Block* block = blocks[0];

    FieldMap fields;

    // We use this vector to make sure the output code writes the fields in the
    // same order as the input code. This vector doesn't have duplicates because
    // the fields are final, so they could only have one sput-object.
    FieldOrder field_order;
    bool can_opt = analyze(block, method->get_class(), &fields, &field_order);

    if (!can_opt) {
      return stats;
    }

    const auto before_size = block->num_opcodes();
    rewrite(&block->get_entries(), fields, field_order);
    const auto after_size = block->num_opcodes();

    always_assert(after_size < before_size);
    stats.insns_removed += before_size - after_size;
    stats.methods_rewritten += 1;
    stats.string_fields_resolved += fields.size();

    TRACE(STR_CAT, 2, "optimize %s from %d to %d\n", SHOW(method), before_size,
          block->num_opcodes());
    return stats;
  }
};

} // namespace

void StringConcatenatorPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* unused */,
                                      PassManager& mgr) {
  const bool DEBUG = false;
  const auto& scope = build_class_scope(stores);
  Stats stats = walk::parallel::reduce_methods<std::nullptr_t, Stats, Scope>(
      scope,
      [](std::nullptr_t, DexMethod* m) -> Stats {
        auto code = m->get_code();
        if (code == nullptr) {
          return Stats{};
        }
        if (!is_clinit(m)) {
          // TODO maybe later? If we expand to non-clinit methods, `analyze()`
          // will have to consider StringBuilders passed in as arguments.
          return Stats{};
        }

        code->build_cfg(/* editable */ true);
        Stats stats = Concatenator{}.run(&code->cfg(), m);
        code->clear_cfg();

        return stats;
      },
      [](Stats a, Stats b) {
        a += b;
        return a;
      },
      [](int) { return nullptr; }, Stats{},
      DEBUG ? 1 : walk::parallel::default_num_threads());
  stats.report(mgr);
}

static StringConcatenatorPass s_pass;
