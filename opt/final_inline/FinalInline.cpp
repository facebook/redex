/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FinalInline.h"

#include <stdio.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Debug.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Walkers.h"

namespace {

class FinalInlineImpl {
 public:
  FinalInlineImpl(const Scope& full_scope, FinalInlinePass::Config& config)
      : m_full_scope(full_scope), m_config(config) {}

  const Scope& m_full_scope;
  FinalInlinePass::Config& m_config;

  bool is_cls_blacklisted(DexClass* clazz) {
    for (auto& type : m_config.black_list_types) {
      if (clazz->get_type() == type) {
        return true;
      }
    }
    for (auto& anno_type : m_config.black_list_annos) {
      if (has_anno(clazz, anno_type)) {
        return true;
      }
    }
    return false;
  }

  std::unordered_set<DexField*> get_called_field_defs(const Scope& scope) {
    std::vector<DexFieldRef*> field_refs;
    walk::methods(scope,
                 [&](DexMethod* method) { method->gather_fields(field_refs); });
    sort_unique(field_refs);
    /* Okay, now we have a complete list of field refs
     * for this particular dex.  Map to the def actually invoked.
     */
    std::unordered_set<DexField*> field_defs;
    for (auto field_ref : field_refs) {
      auto field_def = resolve_field(field_ref);
      if (field_def == nullptr || !field_def->is_concrete()) continue;
      field_defs.insert(field_def);
    }
    return field_defs;
  }

  std::unordered_set<DexField*> get_field_target(
      const Scope& scope, const std::vector<DexField*>& fields) {
    std::unordered_set<DexField*> field_defs = get_called_field_defs(scope);
    std::unordered_set<DexField*> ftarget;
    for (auto field : fields) {
      if (field_defs.count(field) > 0) {
        ftarget.insert(field);
      }
    }
    return ftarget;
  }

  bool keep_member(const std::vector<std::string>& keep_members,
                   const DexField* field) {
    for (auto const& keep : keep_members) {
      if (!strcmp(keep.c_str(), field->get_name()->c_str())) {
        return true;
      }
    }
    return false;
  }

  // returns the number of fields removed
  size_t remove_unused_fields() {
    std::vector<DexField*> moveable_fields;
    std::vector<DexClass*> smallscope;
    uint32_t aflags = ACC_STATIC | ACC_FINAL;
    for (auto clazz : m_full_scope) {
      if (is_cls_blacklisted(clazz)) {
        continue;
      }
      bool found = can_delete(clazz);
      if (!found) {
        auto name = clazz->get_name()->c_str();
        for (const auto& name_prefix : m_config.remove_class_members) {
          if (strstr(name, name_prefix.c_str()) != nullptr) {
            found = true;
            break;
          }
        }
        if (!found) {
          TRACE(FINALINLINE, 2, "Cannot delete: %s\n", SHOW(clazz));
          continue;
        }
      }
      auto sfields = clazz->get_sfields();
      for (auto sfield : sfields) {
        if (keep_member(m_config.keep_class_members, sfield)) continue;
        if ((sfield->get_access() & aflags) != aflags) continue;
        auto value = sfield->get_static_value();
        if (value == nullptr && !is_primitive(sfield->get_type())) continue;
        if (!found && !can_delete(sfield)) continue;

        moveable_fields.push_back(sfield);
        smallscope.push_back(clazz);
      }
    }
    sort_unique(smallscope);

    std::unordered_set<DexField*> field_target =
        get_field_target(m_full_scope, moveable_fields);
    std::unordered_set<DexField*> dead_fields;
    for (auto field : moveable_fields) {
      if (field_target.count(field) == 0) {
        dead_fields.insert(field);
      }
    }
    TRACE(FINALINLINE,
          1,
          "Removable fields %lu/%lu\n",
          dead_fields.size(),
          moveable_fields.size());

    for (auto clazz : smallscope) {
      auto& sfields = clazz->get_sfields();
      sfields.erase(std::remove_if(sfields.begin(),
                                   sfields.end(),
                                   [&](DexField* field) {
                                     return dead_fields.count(field) > 0;
                                   }),
                    sfields.end());
    }
    return smallscope.size();
  }

  bool check_sget(IRInstruction* opfield) {
    auto opcode = opfield->opcode();
    switch (opcode) {
    case OPCODE_SGET:
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_SHORT:
    case OPCODE_SGET_OBJECT:
    case OPCODE_SGET_WIDE:
      return true;
    default:
      return false;
    }
  }

  bool validate_sget(DexMethod* context, IRInstruction* opfield) {
    if (check_sget(opfield)) {
      return true;
    } else {
      auto field = resolve_field(opfield->get_field(), FieldSearch::Static);
      always_assert_log(field->is_concrete(), "Must be a concrete field");
      auto value = field->get_static_value();
      always_assert_log(
          false,
          "Unexpected field type in inline_*sget %s for field %s value %s in "
          "method %s\n",
          SHOW(opfield),
          SHOW(field),
          value != nullptr ? value->show().c_str() : "('nullptr')",
          SHOW(context));
      return false;
    }
  }

  /*
   * There's no "good way" to differentiate blank vs. non-blank finals.
   * So, we just scan the code in the CL-init.  If it's sput there, then it's
   * a blank static, i.e., one without an encoded value field.
   * Lame, agreed, but functional.
   */
  void get_sput_in_clinit(DexClass* clazz,
                          std::unordered_map<DexField*, bool>& blank_statics) {
    auto clinit = clazz->get_clinit();
    if (clinit == nullptr) {
      return;
    }
    always_assert_log(
        is_static(clinit) && is_constructor(clinit),
        "static constructor doesn't have the proper access bits set\n");
    for (auto& mie : InstructionIterable(clinit->get_code())) {
      auto opcode = mie.insn;
      if (opcode->has_field() && is_sput(opcode->opcode())) {
        auto field = resolve_field(opcode->get_field(), FieldSearch::Static);
        if (field == nullptr || !field->is_concrete() ||
            field->get_class() != clazz->get_type()) {
          continue;
        }
        blank_statics[field] = true;
      }
    }
  }

  // returns the total number of inlines
  size_t inline_field_values() {
    std::unordered_set<DexField*> inline_field;
    uint32_t aflags = ACC_STATIC | ACC_FINAL;
    for (auto clazz : m_full_scope) {
      if (is_cls_blacklisted(clazz)) {
        continue;
      }
      std::unordered_map<DexField*, bool> blank_statics;
      get_sput_in_clinit(clazz, blank_statics);
      auto sfields = clazz->get_sfields();
      for (auto sfield : sfields) {
        if ((sfield->get_access() & aflags) != aflags ||
            blank_statics[sfield]) {
          continue;
        }
        auto value = sfield->get_static_value();
        if (value == nullptr && !is_primitive(sfield->get_type())) {
          continue;
        }
        if (value != nullptr && !value->is_evtype_primitive()) {
          continue;
        }
        inline_field.insert(sfield);
      }
    }

    return walk::parallel::reduce_methods<size_t, Scope>(
        m_full_scope,
        [&inline_field, this](DexMethod* m) -> size_t {
          auto* code = m->get_code();
          if (!code) {
            return 0;
          }
          std::vector<IRList::iterator> rewrites;
          auto ii = InstructionIterable(code);
          for (auto it = ii.begin(); it != ii.end(); ++it) {
            auto* insn = it->insn;
            if (!is_sget(insn->opcode())) {
              continue;
            }
            auto field = resolve_field(insn->get_field(), FieldSearch::Static);
            if (field == nullptr || !field->is_concrete()) continue;
            if (inline_field.count(field) == 0) continue;
            if (!validate_sget(m, insn)) continue;
            rewrites.push_back(it.unwrap());
          }
          for (auto it : rewrites) {
            auto* insn = it->insn;
            auto dest = ir_list::move_result_pseudo_of(it)->dest();
            auto field = resolve_field(insn->get_field(), FieldSearch::Static);
            auto value = field->get_static_value();
            auto opcode = value->is_wide() ? OPCODE_CONST_WIDE : OPCODE_CONST;
            uint64_t v =
                value != nullptr ? static_cast<uint64_t>(value->value()) : 0;
            auto newopcode =
                (new IRInstruction(opcode))->set_dest(dest)->set_literal(v);

            code->insert_before(it, newopcode);
            code->remove_opcode(it);
          }
          return rewrites.size();
        },
        [](size_t a, size_t b) { return a + b; });
  }

  /*
   * Verify that we can handle converting the literal contained in the
   * const op into an encoded value.
   */
  bool validate_const_for_encoded_value(IRInstruction* op) {
    if (!is_const(op->opcode())) {
      return false;
    }
    switch (op->opcode()) {
    case OPCODE_CONST:
    case OPCODE_CONST_STRING:
    case OPCODE_CONST_WIDE:
      return true;
    default:
      return false;
    }
  }

  /*
   * Verify that we can convert the field in the sput into an encoded value.
   */
  bool validate_sput_for_encoded_value(const DexClass* clazz,
                                       IRInstruction* insn) {
    if (!(insn->has_field() && is_sput(insn->opcode()))) {
      return false;
    }
    auto field = resolve_field(insn->get_field(), FieldSearch::Static);
    if (field == nullptr || field->get_class() != clazz->get_type() ||
        !is_final(field)) {
      return false;
    }
    // Older DalvikVM handles only two types of classes:
    // https://android.googlesource.com/platform/dalvik.git/+/android-4.3_r3/vm/oo/Class.cpp#3846
    // Without this checking, we may mistakenly accept a "const-string" and
    // "sput-object Ljava/lang/CharSequence;" pair. Such pair can cause a
    // libdvm.so abort with "Bogus static initialization".
    if (insn->opcode() == OPCODE_SPUT_OBJECT &&
        field->get_type() != DexType::get_type("Ljava/lang/String;") &&
        field->get_type() != DexType::get_type("Ljava/lang/Class;")) {
      TRACE(FINALINLINE,
            8,
            "Validating: reject SPUT_OBJECT with %s\n",
            SHOW(field));
      return false;
    }
    return true;
  }

  /*
   * Attempt to replace the clinit with corresponding encoded values.
   */
  bool try_replace_clinit(DexClass* clazz, DexMethod* clinit) {
    std::vector<std::pair<IRInstruction*, IRInstruction*>> const_sputs;
    auto ii = InstructionIterable(clinit->get_code());
    auto end = ii.end();
    // Verify the entire opcodes in this clinit are (const, sput)* pairs
    // followed by return-void.
    for (auto it = ii.begin(); it != end; ++it) {
      auto first_op = it->insn;
      ++it;
      if (it == end) {
        if (first_op->opcode() != OPCODE_RETURN_VOID) {
          TRACE(FINALINLINE,
                8,
                "Can't replace: %s :: Last opcode is not return void\n",
                SHOW(clinit));
          return false;
        }
        break;
      }
      auto sput_op = it->insn;
      bool condition_const = validate_const_for_encoded_value(first_op);
      bool condition_sput =
          condition_const && validate_sput_for_encoded_value(clazz, sput_op);
      bool condition_register_match =
          condition_sput && first_op->dest() == sput_op->src(0);
      if (!condition_register_match) {
        TRACE(FINALINLINE,
              8,
              "Can't replace: %s :: Can't validate :: const :: %s :: sput :: "
              "%s :: register match :: %s\n",
              SHOW(clinit),
              condition_const ? "True" : "False",
              condition_sput ? "True" : "False",
              condition_register_match ? "True" : "False");
        TRACE(FINALINLINE, 8, "%s\n", SHOW(clinit->get_code()));
        return false;
      }
      const_sputs.emplace_back(first_op, sput_op);
    }

    // Attach encoded values and remove the clinit
    TRACE(FINALINLINE,
          8,
          "Replacing <clinit> %s: %lu pairs...\n",
          SHOW(clinit),
          const_sputs.size());
    for (auto& pair : const_sputs) {
      auto const_op = pair.first;
      auto sput_op = pair.second;
      auto field = resolve_field(sput_op->get_field(), FieldSearch::Static);
      DexEncodedValue* ev;
      if (const_op->opcode() == OPCODE_CONST_STRING) {
        TRACE(FINALINLINE,
              8,
              "- String Field: %s, \"%s\"\n",
              SHOW(field),
              SHOW(const_op->get_string()));
        ev = new DexEncodedValueString(const_op->get_string());
      } else {
        TRACE(FINALINLINE,
              9,
              "- Integer Field: %s, %lu\n",
              SHOW(field),
              static_cast<uint64_t>(const_op->get_literal()));
        ev = DexEncodedValue::zero_for_type(field->get_type());
        ev->value(static_cast<uint64_t>(const_op->get_literal()));
      }
      field->make_concrete(field->get_access(), ev);
    }
    clazz->remove_method(clinit);

    return true;
  }

  size_t replace_encodable_clinits() {
    size_t nreplaced = 0;
    size_t ntotal = 0;
    for (auto clazz : m_full_scope) {
      if (is_cls_blacklisted(clazz)) {
        continue;
      }
      auto clinit = clazz->get_clinit();
      if (clinit == nullptr) {
        continue;
      }
      ntotal++;
      if (try_replace_clinit(clazz, clinit)) {
        TRACE(FINALINLINE,
              2,
              "Replaced clinit for class %s with encoded values\n",
              SHOW(clazz));
        nreplaced++;
      }
    }
    TRACE(FINALINLINE,
          1,
          "Replaced %lu/%lu clinits with encoded values\n",
          nreplaced,
          ntotal);
    return nreplaced;
  }

  // Check that source register is either overwritten or isn't used
  // again. This ensures we can safely remove the opcode pair without
  // breaking future instructions that rely on the value of the source
  // register.  Yes, this means we're N^2 in theory, but hopefully in
  // practice we don't approach that.
  bool reg_reused(uint16_t reg,
                  const ir_list::InstructionIterator& it,
                  const ir_list::InstructionIterator& end) {
    for (auto jt = std::next(it, 2); jt != end; ++jt) {
      auto insn = jt->insn;

      // Check if the source register is overwritten
      if (insn->dests_size() > 0) {
        if (insn->dest() == reg ||
            (insn->dest_is_wide() && insn->dest() + 1 == reg)) {
          return false;
        }
      }

      // Check if the source register is reused as the source for another
      // instruction
      for (size_t r = 0; r < insn->srcs_size(); ++r) {
        if (insn->src(r) == reg ||
            (insn->src_is_wide(r) && insn->src(r) + 1 == reg)) {
          return true;
        }
      }
    }
    return false;
  }

  /*
   * Attempt to propagate constant values that are known only after the APK has
   * been created. Our build process can result in situation where javac sees
   * something resembling:
   *
   *   class Parent {
   *     public static int CONST = 0;
   *   }
   *
   *   class Child {
   *     public static final CONST = Parent.CONST;
   *   }
   *
   * Parent.CONST is not final, so javac cannot perform constant propagation.
   * However, Parent.CONST may be marked final when we package the APK, thereby
   * opening up an opportunity for constant propagation by redex.
   */
  size_t propagate_constants() {
    // Build dependency map (static -> [statics] that depend on it)
    TRACE(FINALINLINE, 2, "Building dependency map\n");
    std::unordered_map<DexField*, std::vector<FieldDependency>> deps =
        find_dependencies(m_full_scope);

    // Collect static finals whose values are known. These serve as the starting
    // point of the dependency resolution process.
    std::deque<DexField*> resolved;
    for (auto clazz : m_full_scope) {
      std::unordered_map<DexField*, bool> blank_statics;
      // TODO: Should we allow static finals that are initialized w/ const,
      // sput?
      get_sput_in_clinit(clazz, blank_statics);
      auto sfields = clazz->get_sfields();
      for (auto sfield : sfields) {
        if (!(is_static(sfield) && is_final(sfield)) || blank_statics[sfield]) {
          continue;
        }
        resolved.push_back(sfield);
      }
    }

    // Resolve dependencies (tsort)
    size_t nresolved = 0;
    while (!resolved.empty()) {
      auto cur = resolved.front();
      TRACE(FINALINLINE, 2, "Resolving deps of %s\n", SHOW(cur));
      resolved.pop_front();
      if (deps.count(cur) == 0) {
        continue;
      }
      auto val = cur->get_static_value();
      for (const auto& dep : deps[cur]) {
        dep.field->make_concrete(dep.field->get_access(), val);
        auto code = dep.clinit->get_code();
        TRACE(FINALINLINE, 5, "Removing %s\n", SHOW(dep.sget->insn));
        TRACE(FINALINLINE, 5, "Removing %s\n", SHOW(dep.sput->insn));
        code->remove_opcode(dep.sget);
        code->remove_opcode(dep.sput);
        ++nresolved;
        resolved.push_back(dep.field);
        TRACE(FINALINLINE, 2, "Resolved field %s\n", SHOW(dep.field));
      }
    }
    TRACE(FINALINLINE,
          1,
          "Resolved %lu static finals via const prop\n",
          nresolved);
    return nresolved;
  }

  std::unordered_map<DexField*, std::vector<FieldDependency>> find_dependencies(
      const Scope& scope) {
    std::unordered_map<DexField*, std::vector<FieldDependency>> result;
    for (auto clazz : scope) {
      if (is_cls_blacklisted(clazz)) {
        continue;
      }
      auto clinit = clazz->get_clinit();
      if (clinit == nullptr) {
        continue;
      }
      find_dependencies(clazz, clinit, result);
    }
    return result;
  };

  void find_dependencies(
      const DexClass* clazz,
      DexMethod* clinit,
      std::unordered_map<DexField*, std::vector<FieldDependency>>& deps) {
    auto code = clinit->get_code();
    auto ii = InstructionIterable(code);
    auto end = ii.end();
    for (auto it = ii.begin(); it != end; ++it) {
      // Check for sget from static final
      if (!it->insn->has_field()) {
        continue;
      }
      auto sget_op = it->insn;
      IRList::iterator sget_op_iterator = it.unwrap();
      if (!check_sget(sget_op)) {
        continue;
      }
      auto src_field = resolve_field(sget_op->get_field(), FieldSearch::Static);
      if ((src_field == nullptr) ||
          !(is_static(src_field) && is_final(src_field))) {
        continue;
      }

      auto sget_move_result = ir_list::move_result_pseudo_of(it.unwrap());
      // skip the move-result-pseudo
      ++it;

      // Check for sput to static final
      auto next_it = std::next(it);
      auto next_insn = next_it->insn;
      auto sput_op_iterator = next_it.unwrap();
      if (!validate_sput_for_encoded_value(clazz, next_insn)) {
        continue;
      }
      auto sput_op = next_insn;
      auto dst_field = resolve_field(sput_op->get_field(), FieldSearch::Static);
      if (!(is_static(dst_field) && is_final(dst_field))) {
        continue;
      }

      // Check that dst register for sget is src register for sput
      if (sget_move_result->dest() != sput_op->src(0)) {
        continue;
      }

      if (reg_reused(sget_move_result->dest(), it, end) ||
          (sget_op->opcode() == OPCODE_SGET_WIDE &&
           reg_reused(sget_move_result->dest() + 1, it, end))) {
        TRACE(FINALINLINE,
              2,
              "Cannot propagate %s to %s. Source register reused.\n",
              SHOW(src_field),
              SHOW(dst_field));
        continue;
      }

      // Yay, we found a dependency!
      TRACE(FINALINLINE,
            2,
            "Field %s depends on %s\n",
            SHOW(dst_field),
            SHOW(src_field));
      deps[src_field].emplace_back(
          clinit, sget_op_iterator, sput_op_iterator, dst_field);
    }
  }
};
} // namespace

size_t FinalInlinePass::propagate_constants_for_test(Scope& scope,
                                                     bool inline_string_fields,
                                                     bool inline_wide_fields) {
  FinalInlinePass::Config config{};

  FinalInlineImpl impl(scope, config);
  return impl.propagate_constants();
}

void FinalInlinePass::run_pass(DexStoresVector& stores,
                               ConfigFiles& conf,
                               PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(FINALINLINE,
          1,
          "FinalInlinePass not run because no ProGuard configuration was "
          "provided.");
    return;
  }
  auto scope = build_class_scope(stores);

  FinalInlineImpl impl(scope, m_config);
  if (m_config.replace_encodable_clinits) {
    auto nreplaced = impl.replace_encodable_clinits();
    mgr.incr_metric("encodable_clinits_replaced", nreplaced);
  }

  if (m_config.propagate_static_finals) {
    auto nresolved = impl.propagate_constants();
    mgr.incr_metric("static_finals_resolved", nresolved);
  }

  // Constprop may resolve statics that were initialized via clinit. This opens
  // up another opportunity to remove (potentially empty) clinits.
  if (m_config.replace_encodable_clinits) {
    auto nreplaced = impl.replace_encodable_clinits();
    mgr.incr_metric("encodable_clinits_replaced", nreplaced);
  }

  size_t num_finals_inlined = impl.inline_field_values();
  size_t num_removed_fields = impl.remove_unused_fields();

  mgr.incr_metric("num_finals_inlined", num_finals_inlined);
  mgr.incr_metric("num_removed_fields", num_removed_fields);
}

void FinalInlinePass::inline_fields(const Scope& scope) {
  FinalInlinePass::Config config{};

  FinalInlineImpl impl(scope, config);
  impl.inline_field_values();
  impl.remove_unused_fields();
}

void FinalInlinePass::inline_fields(const Scope& scope,
                                    FinalInlinePass::Config& config) {
  FinalInlineImpl impl(scope, config);
  impl.inline_field_values();
}

const std::unordered_map<DexField*, std::vector<FieldDependency>>
FinalInlinePass::find_dependencies(const Scope& scope,
                                   DexMethod* method,
                                   FinalInlinePass::Config& config) {
  FinalInlineImpl impl(scope, config);
  std::unordered_map<DexField*, std::vector<FieldDependency>> result;
  impl.find_dependencies(type_class(method->get_class()), method, result);
  return result;
}

static FinalInlinePass s_pass;
