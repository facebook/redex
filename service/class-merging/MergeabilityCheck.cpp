/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MergeabilityCheck.h"

#include "LiveRange.h"
#include "Model.h"
#include "ReachableClasses.h"
#include "RefChecker.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

using namespace class_merging;

MergeabilityChecker::MergeabilityChecker(const Scope& scope,
                                         const ModelSpec& spec,
                                         const RefChecker& ref_checker,
                                         const TypeSet& generated)
    : m_scope(scope),
      m_spec(spec),
      m_ref_checker(ref_checker),
      m_generated(generated),
      m_const_class_safe_types(spec.const_class_safe_types) {}

void MergeabilityChecker::exclude_unsupported_cls_property(
    TypeSet& non_mergeables) {
  for (const auto& type : m_spec.merging_targets) {
    const auto& cls = type_class(type);
    if (!can_delete(cls)) {
      non_mergeables.insert(type);
      TRACE(CLMG, 5, "Cannot delete %s", SHOW(type));
      continue;
    }
    // Why uninstantiable classes are not mergeable?
    // Class Merging is good at merging virtual methods horizontally by
    // supporting virtual dispatches. There's no benefit to merge uninstantiable
    // classes and no proper way to merge uninstatiable and instantiable classes
    // together. Exclude the uninstantiable classes from ClassMerging and
    // RemoveUnreachablePass should properly handle parts of them.
    bool has_ctor = false;
    for (const auto& method : cls->get_dmethods()) {
      if (is_constructor(method) && method::is_init(method)) {
        has_ctor = true;
        break;
      }
    }
    if (!has_ctor) {
      non_mergeables.insert(type);
      TRACE(CLMG, 5, "Has no ctor %s", SHOW(type));
    }
    // We do not support merging abstract and non-abstract classes together.
    if (is_abstract(cls)) {
      non_mergeables.insert(type);
      TRACE(CLMG, 5, "Is abstract %s", SHOW(type));
    }
  }
}

TypeSet MergeabilityChecker::exclude_unsupported_bytecode_refs_for(
    DexMethod* method) {
  TypeSet non_mergeables;
  auto code = method->get_code();
  if (!code || m_generated.count(method->get_class())) {
    return non_mergeables;
  }

  bool has_type_tag = m_spec.has_type_tag();
  std::vector<std::pair<IRInstruction*, const DexType*>>
      const_classes_to_verify;
  auto& cfg = code->cfg();
  for (const auto& mie : InstructionIterable(cfg)) {
    auto insn = mie.insn;

    // If we have a pure method ref on a mergeable type (the class component is
    // mergeable), we do not merge the type.
    // 1. We cannot properly update type references on pure method refs.
    // 2. We rely the ResolveRefsPass to resolve all pure method refs before
    // running Class Merging. However, there are rare exceptions where resolving
    // method refs to external cannot be done. In this case, it's safer not to
    // merge types with existing pure method refs on the type.
    if (insn->has_method() && !insn->get_method()->is_def()) {
      auto meth_ref = insn->get_method();
      auto type = meth_ref->get_class();
      if (m_spec.merging_targets.count(type) > 0) {
        TRACE(CLMG, 5, "[non mergeable] referenced by pure ref %s in %s",
              SHOW(meth_ref), SHOW(method));
        non_mergeables.insert(type);
      }
      continue;
    }

    // The presence of type-like strings can indicate that types are used by
    // reflection, and then it's not safe to merge those types.
    if (m_spec.exclude_type_like_strings() &&
        opcode::is_const_string(insn->opcode())) {
      const DexString* str = insn->get_string();
      std::string class_name = java_names::external_to_internal(str->str());
      DexType* maybe_type = DexType::get_type(class_name);
      if (maybe_type && m_spec.merging_targets.count(maybe_type) > 0) {
        non_mergeables.insert(maybe_type);
        TRACE(CLMG, 5,
              "[non mergeable] type like const string unsafe: %s in %s",
              SHOW(insn), SHOW(method));
      }
      continue;
    }

    // Java language level enforcement recommended!
    //
    // For mergeables with type tags, it is not safe to merge those
    // referenced by CONST_CLASS, since we will lose granularity as we can't map
    // to the old type anymore.
    if (has_type_tag && !opcode::is_const_class(insn->opcode())) {
      continue;
    }

    // Java language level enforcement recommended!
    //
    // For mergeables without a type tag, it is not safe to merge
    // those used in an INSTANCE_OF, since we might lose granularity.
    //
    // Example where both <type_0> and <type_1> have the same shape
    // (so end
    //        up in the same merger)
    //
    //    INSTANCE_OF <v_result>, <v_obj> <type_0>
    //    then label:
    //      CHECK_CAST <type_0>
    //    else labe:
    //      CHECK_CAST <type_1>
    if (!has_type_tag && !opcode::is_instance_of(insn->opcode())) {
      continue;
    }

    const auto* type = type::get_element_type_if_array(insn->get_type());
    if (m_spec.merging_targets.count(type) > 0) {
      if (!opcode::is_const_class(insn->opcode()) ||
          m_const_class_safe_types.empty()) {
        non_mergeables.insert(type);
      } else {
        // To verify the usages
        const_classes_to_verify.emplace_back(insn, type);
      }
    }
  }

  if (const_classes_to_verify.empty()) {
    return non_mergeables;
  }

  live_range::MoveAwareChains chains(cfg);
  live_range::DefUseChains du_chains = chains.get_def_use_chains();

  for (const auto& pair : const_classes_to_verify) {
    auto const_class_insn = pair.first;
    auto referenced_type = pair.second;
    auto use_set = du_chains[const_class_insn];
    for (const auto use : use_set) {
      auto use_insn = use.insn;
      if (opcode::is_a_move(use_insn->opcode())) {
        // Ignore moves
        break;
      }
      if (!use_insn->has_method()) {
        TRACE(CLMG, 5, "[non mergeable] const class unsafe use @ %s in %s",
              SHOW(use_insn), SHOW(method));
        non_mergeables.insert(referenced_type);
        break;
      }
      auto callee = use_insn->get_method();
      auto callee_type = callee->get_class();
      if (!m_const_class_safe_types.count(callee_type)) {
        TRACE(CLMG, 5, "[non mergeable] const class unsafe callee %s in %s",
              SHOW(callee), SHOW(method));
        non_mergeables.insert(referenced_type);
        break;
      }
    }
  }

  return non_mergeables;
}

void MergeabilityChecker::exclude_unsupported_bytecode(
    TypeSet& non_mergeables) {
  TypeSet non_mergeables_opcode =
      walk::parallel::methods<TypeSet, MergeContainers<TypeSet>>(
          m_scope, [this](DexMethod* meth) {
            return exclude_unsupported_bytecode_refs_for(meth);
          });

  non_mergeables.insert(non_mergeables_opcode.begin(),
                        non_mergeables_opcode.end());
}

void MergeabilityChecker::exclude_static_fields(TypeSet& non_mergeables) {
  static const DexType* string_type = type::java_lang_String();

  if (m_spec.merge_types_with_static_fields) {
    return;
  }

  walk::fields(m_scope, [&non_mergeables, this](DexField* field) {
    if (m_spec.merging_targets.count(field->get_class())) {
      if (is_static(field)) {
        auto rtype = type::get_element_type_if_array(field->get_type());
        if (!type::is_primitive(rtype) && rtype != string_type) {
          // If the type is either non-primitive or a list of
          // non-primitive types (excluding Strings), then exclude it as
          // we might change the initialization order.
          TRACE(CLMG,
                5,
                "[non mergeable] %s as it contains a non-primitive "
                "static field",
                SHOW(field->get_class()));
          non_mergeables.emplace(field->get_class());
        }
      }
    }
  });
}

void MergeabilityChecker::exclude_unsafe_sdk_and_store_refs(
    TypeSet& non_mergeables) {
  const auto mog = method_override_graph::build_graph(m_scope);
  for (auto type : m_spec.merging_targets) {
    if (non_mergeables.count(type)) {
      continue;
    }
    auto cls = type_class(type);
    if (!m_ref_checker.check_class(cls, mog)) {
      non_mergeables.insert(type);
    }
    if (!m_spec.include_primary_dex && m_ref_checker.is_in_primary_dex(type)) {
      non_mergeables.insert(type);
    }
  }
}

TypeSet MergeabilityChecker::get_non_mergeables() {
  TypeSet non_mergeables;
  size_t prev_size = 0;

  exclude_unsupported_cls_property(non_mergeables);
  TRACE(CLMG, 4, "Non mergeables (no delete) %zu", non_mergeables.size());
  prev_size = non_mergeables.size();

  exclude_unsupported_bytecode(non_mergeables);
  TRACE(CLMG,
        4,
        "Non mergeables (opcodes) %zu",
        non_mergeables.size() - prev_size);
  prev_size = non_mergeables.size();

  exclude_static_fields(non_mergeables);
  TRACE(CLMG,
        4,
        "Non mergeables (static fields) %zu",
        non_mergeables.size() - prev_size);
  prev_size = non_mergeables.size();

  exclude_unsafe_sdk_and_store_refs(non_mergeables);
  TRACE(CLMG,
        4,
        "Non mergeables (unsafe refs) %zu",
        non_mergeables.size() - prev_size);
  return non_mergeables;
}
