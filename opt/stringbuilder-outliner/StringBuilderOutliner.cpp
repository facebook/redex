/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringBuilderOutliner.h"

#include <unordered_map>
#include <unordered_set>

#include "ConcurrentContainers.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

using namespace stringbuilder_outliner;

FixpointIterator::FixpointIterator(const cfg::ControlFlowGraph& cfg)
    : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
      m_stringbuilder(DexType::get_type("Ljava/lang/StringBuilder;")),
      m_stringbuilder_no_param_init(
          DexMethod::get_method("Ljava/lang/StringBuilder;.<init>:()V")),
      m_stringbuilder_init_with_string(DexMethod::get_method(
          "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")),
      m_append_str(DexString::get_string("append")) {
  always_assert(m_stringbuilder != nullptr);
  always_assert(m_stringbuilder_init_with_string != nullptr);
  always_assert(m_append_str != nullptr);

  m_immutable_types.emplace(type::_boolean());
  m_immutable_types.emplace(type::_char());
  m_immutable_types.emplace(type::_double());
  m_immutable_types.emplace(type::_float());
  m_immutable_types.emplace(type::_int());
  m_immutable_types.emplace(type::_long());
  m_immutable_types.emplace(type::java_lang_String());
}

/*
 * Only include constructors that we know are safe for our outlining scheme. In
 * particular, we want to exclude some constructors:
 *
 * 1) The constructor that takes an integer argument will throw if that number
 * is negative. Our outlining transformation would drop that integer argument
 * and could therefore change observable behavior.
 *
 * 2) The constructor that takes a CharSequence is not accepted because
 * the CharSequence interface can be implemented by mutable types. Mutable types
 * make outlining tricky: see the `mutableCharSequence` test in the
 * StringBuilderOutlinerTest suite for an example.
 */
bool FixpointIterator::is_eligible_init(const DexMethodRef* method) const {
  return method == m_stringbuilder_no_param_init ||
         method == m_stringbuilder_init_with_string;
}

/*
 * Check if it is a method of the form StringBuilder.append(<immutable>).
 */
bool FixpointIterator::is_eligible_append(const DexMethodRef* method) const {
  auto type_list = method->get_proto()->get_args();
  return method->get_name() == m_append_str && type_list->size() == 1 &&
         m_immutable_types.count(type_list->at(0));
}

void FixpointIterator::analyze_instruction(const IRInstruction* insn,
                                           Environment* env) const {
  namespace ptrs = local_pointers;

  ptrs::escape_heap_referenced_objects(insn, env);

  auto op = insn->opcode();
  if (opcode::is_an_invoke(op) &&
      insn->get_method()->get_class() == m_stringbuilder) {
    auto method = insn->get_method();
    if (method == m_stringbuilder_init_with_string ||
        is_eligible_append(method)) {
      env->update_store(
          insn->src(0),
          [&](const IRInstruction* ptr, BuilderStore::Domain* store) {
            store->update(ptr, [&](const BuilderDomain& builder) {
              auto copy = builder;
              copy.add_operation(insn);
              return copy;
            });
          });
      if (method->get_name() == m_append_str) {
        env->set_pointers(RESULT_REGISTER, env->get_pointers(insn->src(0)));
      }
    } else if (!is_eligible_init(method)) {
      TRACE(STRBUILD, 5, "Unhandled SB method: %s", SHOW(insn));
      ptrs::default_instruction_handler(insn, env);
    }
  } else if (op == OPCODE_NEW_INSTANCE && insn->get_type() == m_stringbuilder) {
    env->set_fresh_pointer(RESULT_REGISTER, insn);
  } else {
    ptrs::default_instruction_handler(insn, env);
  }
}

Outliner::Outliner(Config config)
    : m_config(config),
      m_append_str(DexString::get_string("append")),
      m_stringbuilder(DexType::get_type("Ljava/lang/StringBuilder;")),
      m_stringbuilder_default_ctor(
          DexMethod::get_method("Ljava/lang/StringBuilder;.<init>:()V")),
      m_stringbuilder_capacity_ctor(
          DexMethod::get_method("Ljava/lang/StringBuilder;.<init>:(I)V")),
      m_stringbuilder_tostring(DexMethod::get_method(
          "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")) {
  always_assert(m_append_str);
  always_assert(m_stringbuilder);
  always_assert(m_stringbuilder_default_ctor);
  always_assert(m_stringbuilder_capacity_ctor);
  always_assert(m_stringbuilder_tostring);
}

InstructionSet Outliner::find_tostring_instructions(
    const cfg::ControlFlowGraph& cfg) const {
  std::unordered_set<const IRInstruction*> instructions;
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
          insn->get_method() == m_stringbuilder_tostring) {
        instructions.emplace(insn);
      }
    }
  }
  return instructions;
}

/*
 * Gather the BuilderStates corresponding to StringBuilders whose state we can
 * accurately model for outlining purposes.
 */
BuilderStateMap Outliner::gather_builder_states(
    const cfg::ControlFlowGraph& cfg,
    const InstructionSet& tostring_instructions) const {
  BuilderStateMap tostring_instruction_to_state;
  FixpointIterator fp_iter(cfg);
  fp_iter.run(Environment());
  for (auto* block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (tostring_instructions.count(insn)) {
        const auto& pointers = env.get_pointers(insn->src(0));
        if (!pointers.is_value() || pointers.elements().size() != 1) {
          TRACE(STRBUILD, 5, "Did not get single pointer for %s", SHOW(insn));
          continue;
        }
        const auto& pointer = *pointers.elements().begin();
        const auto& builder = env.get_store().get(pointer);
        const auto& state_opt = builder.state();
        if (!state_opt) {
          TRACE(STRBUILD, 5, "Did not get state for %s", SHOW(insn));
        } else {
          tostring_instruction_to_state.emplace_back(insn, *state_opt);
        }
      }
      fp_iter.analyze_instruction(insn, &env);
    }
  }
  return tostring_instruction_to_state;
}

/*
 * Gather the types of the values that the StringBuilder instance is
 * concatenating.
 */
const DexTypeList* Outliner::typelist_from_state(
    const BuilderState& state) const {
  DexTypeList::ContainerType args;
  for (auto* insn : state) {
    auto method = insn->get_method();
    args.emplace_back(method->get_proto()->get_args()->at(0));
  }
  return DexTypeList::make_type_list(std::move(args));
}

void Outliner::gather_outline_candidate_typelists(
    const BuilderStateMap& tostring_instruction_to_state) {
  for (const auto& p : tostring_instruction_to_state) {
    const auto& state = p.second;
    auto typelist = typelist_from_state(state);
    m_outline_typelists.update(
        typelist,
        [](const DexTypeList*, size_t& n, bool /* exists */) { ++n; });
  }
}

void Outliner::analyze(IRCode& code) {
  code.build_cfg(/* editable */ false); // Not editable because of T42743620
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();

  // Do a quick one-pass scan to see if the method has any instructions that may
  // be outlinable. Only do the more expensive fixpoint calculations if the
  // method passes this check.
  auto tostring_instructions = find_tostring_instructions(cfg);
  if (tostring_instructions.empty()) {
    return;
  }

  auto tostring_instruction_to_state =
      gather_builder_states(cfg, tostring_instructions);

  gather_outline_candidate_typelists(tostring_instruction_to_state);

  m_builder_state_maps.emplace(&code, std::move(tostring_instruction_to_state));
}

/*
 * Don't create helpers if:
 * 1) They only have a few use sites -- the performance/size overheads of
 * outlining may not be worth it.
 * 2) A long list of parameters would be required. Calls to these methods could
 * significantly increase register pressure in the caller.
 */
void Outliner::create_outline_helpers(DexStoresVector* stores) {
  auto outline_helper_cls =
      DexType::make_type("Lcom/redex/OutlinedStringBuilders;");
  auto concat_str = DexString::make_string("concat");
  auto string_ty = DexType::make_type("Ljava/lang/String;");

  ClassCreator cc(outline_helper_cls);
  cc.set_super(type::java_lang_Object());
  bool did_create_helper{false};
  for (const auto& p : m_outline_typelists) {
    const auto* typelist = p.first;
    auto count = p.second;

    if (count < m_config.min_outline_count ||
        typelist->size() > m_config.max_outline_length) {
      // TODO: filter out length zero/one states?
      continue;
    }
    TRACE(STRBUILD, 3,
          "Outlining %lu StringBuilders of length %lu with typelist %s", count,
          typelist->size(), SHOW(typelist));
    m_stats.stringbuilders_removed += count;
    m_stats.operations_removed += count * typelist->size();

    if (m_outline_helpers.count(typelist) != 0) {
      continue;
    }
    m_stats.helper_methods_created += 1;

    auto helper =
        DexMethod::make_method(outline_helper_cls, concat_str,
                               DexProto::make_proto(string_ty, typelist))
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    helper->set_code(create_outline_helper_code(helper));
    helper->set_deobfuscated_name(show(helper));
    cc.add_method(helper);
    did_create_helper = true;
    m_outline_helpers.emplace(typelist, helper);
  }

  if (did_create_helper) {
    auto& dexen = (*stores)[0].get_dexen()[0];
    dexen.push_back(cc.create());
  }
}

/*
 * Given a method with a proto like `concat(String, int, String)`, generate
 * IRCode equivalent to the following Java:
 *
 *   String concat(String a, int b, String c) {
 *     StringBuilder sb = new StringBuilder();
 *     sb.append(a);
 *     sb.append(b);
 *     sb.append(c);
 *     return sb.toString();
 *   }
 */
std::unique_ptr<IRCode> Outliner::create_outline_helper_code(
    DexMethod* method) const {
  using namespace dex_asm;

  auto typelist = method->get_proto()->get_args();
  auto code = std::make_unique<IRCode>(method, 1);
  code->push_back(dasm(OPCODE_NEW_INSTANCE, m_stringbuilder));
  code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}));
  code->push_back(
      dasm(OPCODE_INVOKE_DIRECT, m_stringbuilder_default_ctor, {0_v}));
  auto param_insns_it =
      InstructionIterable(code->get_param_instructions()).begin();
  for (size_t i = 0; i < typelist->size(); ++i, ++param_insns_it) {
    auto ty = typelist->at(i);
    auto reg = param_insns_it->insn->dest();
    auto append_method = DexMethod::get_method(
        m_stringbuilder, m_append_str,
        DexProto::make_proto(m_stringbuilder,
                             DexTypeList::make_type_list({ty})));
    always_assert_log(append_method, "Could not find append for %s", SHOW(ty));
    code->push_back((new IRInstruction(OPCODE_INVOKE_VIRTUAL))
                        ->set_method(append_method)
                        ->set_srcs_size(2)
                        ->set_src(0, 0)
                        ->set_src(1, reg));
  }
  code->push_back(dasm(OPCODE_INVOKE_VIRTUAL, m_stringbuilder_tostring, {0_v}));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {0_v}));
  code->push_back(dasm(OPCODE_RETURN_OBJECT, {0_v}));
  return code;
}

static IROpcode move_for_type(const DexType* ty) {
  if (!type::is_primitive(ty)) {
    return OPCODE_MOVE_OBJECT;
  } else if (type::is_wide_type(ty)) {
    return OPCODE_MOVE_WIDE;
  } else {
    return OPCODE_MOVE;
  }
}

static IROpcode invoke_for_method(const DexMethod* method) {
  if (is_static(method)) {
    return OPCODE_INVOKE_STATIC;
  } else if (method->is_virtual()) {
    return OPCODE_INVOKE_VIRTUAL;
  } else {
    return OPCODE_INVOKE_DIRECT;
  }
}

/*
 * Convert a sequence of instructions like
 *
 *   invoke-virtual {v0, v1} StringBuilder.append(String)
 *   invoke-virtual {v0, v2} StringBuilder.append(String)
 *   ...
 *   invoke-virtual {v0, vN} StringBuilder.append(String)
 *   invoke-virtual {v0} StringBuilder.toString()
 *
 * into
 *
 *   move-object v1, vN + 1
 *   invoke-virtual {v0, v1} StringBuilder.append(String)
 *   move-object v2, vN + 2
 *   invoke-virtual {v0, v2} StringBuilder.append(String)
 *   ...
 *   move-object v2, vN + N
 *   invoke-virtual {v0, vN} StringBuilder.append(String)
 *   invoke-static {vN + 1, vN + 2, ..., vN + N} OutlinedStringBuilders.concat()
 *
 * It is anticipated that the now-redundant StringBuilder.append() calls will
 * be removed by a later run of ObjectSensitiveDcePass, and that most of the
 * move instructions created here will be eliminated as part of move coalescing
 * during register allocation.
 */
void Outliner::transform(IRCode* code) {
  if (m_builder_state_maps.count(code) == 0) {
    return;
  }
  const auto& tostring_instruction_to_state = m_builder_state_maps.at(code);

  std::unordered_map<const IRInstruction*, IRInstruction*> insns_to_insert;
  std::unordered_map<const IRInstruction*, IRInstruction*> insns_to_replace;
  for (const auto& p : tostring_instruction_to_state) {
    const auto* tostring_insn = p.first;
    const auto& state = p.second;
    const auto* typelist = typelist_from_state(state);
    if (m_outline_helpers.count(typelist) == 0) {
      continue;
    }
    auto* outline_helper = m_outline_helpers.at(typelist);
    auto invoke_outlined = new IRInstruction(invoke_for_method(outline_helper));
    invoke_outlined->set_method(outline_helper);
    invoke_outlined->set_srcs_size(typelist->size());

    size_t idx{0};
    for (auto* insn : state) {
      reg_t reg;
      if (insns_to_insert.count(insn)) {
        // An instruction can occur in more than one BuilderState if the
        // corresponding StringBuilder instance is used in both sides of a
        // conditional branch.
        reg = insns_to_insert.at(insn)->dest();
      } else {
        auto ty = insn->get_method()->get_proto()->get_args()->at(0);
        reg = type::is_wide_type(ty) ? code->allocate_wide_temp()
                                     : code->allocate_temp();
        auto move = (new IRInstruction(move_for_type(ty)))
                        ->set_src(0, insn->src(1))
                        ->set_dest(reg);
        insns_to_insert.emplace(insn, move);
      }

      invoke_outlined->set_src(idx, reg);
      ++idx;
    }

    insns_to_replace.emplace(tostring_insn, invoke_outlined);
  }

  apply_changes(insns_to_insert, insns_to_replace, code);
}

/*
 * The StringBuilder analysis tracks and describes transformations in terms of
 * IRInstructions, but efficient insertion / removal of IRInstructions requires
 * knowing their corresponding IRList iterators. This method does one pass to
 * obtain those iterators before doing the appropriate transforms.
 */
void Outliner::apply_changes(
    const std::unordered_map<const IRInstruction*, IRInstruction*>&
        insns_to_insert,
    const std::unordered_map<const IRInstruction*, IRInstruction*>&
        insns_to_replace,
    IRCode* code) {
  auto& cfg = code->cfg();
  std::vector<std::pair<IRList::iterator, IRInstruction*>> to_insert;
  std::vector<std::pair<IRList::iterator, IRInstruction*>> to_replace;
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      auto it = code->iterator_to(mie);
      if (insns_to_insert.count(mie.insn)) {
        to_insert.emplace_back(it, insns_to_insert.at(insn));
      }
      if (insns_to_replace.count(insn)) {
        to_replace.emplace_back(it, insns_to_replace.at(insn));
      }
    }
  }

  for (const auto& p : to_insert) {
    code->insert_before(p.first, p.second);
  }
  for (const auto& p : to_replace) {
    code->insert_before(p.first, p.second);
    code->remove_opcode(p.first);
  }
}

void StringBuilderOutlinerPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  auto scope = build_class_scope(stores);
  Outliner outliner(m_config);
  // 1) Determine which methods have candidates for outlining.
  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    outliner.analyze(code);
  });
  // 2) Determine which candidates occur frequently enough to be worth
  // outlining. Build the corresponding outline helper functions.
  outliner.create_outline_helpers(&stores);
  // 3) Actually do the outlining.
  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    outliner.transform(&code);
  });

  mgr.incr_metric("stringbuilders_removed",
                  outliner.get_stats().stringbuilders_removed);
  mgr.incr_metric("operations_removed",
                  outliner.get_stats().operations_removed);
  mgr.incr_metric("helper_methods_created",
                  outliner.get_stats().helper_methods_created);
}

static StringBuilderOutlinerPass s_pass;
