/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexUtil.h"
#include "MethodDedup.h"
#include "MethodOverrideGraph.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
// Only look at small methods. Increase the limit does not match more
// identical code in practise.
constexpr const uint32_t MAX_NUM_INSTRUCTIONS = 20;

/**
 * Allow code without invoke-super and with less than MAX_NUM_INSTRUCTIONS
 * instructions.
 */
bool eligible_code(const IRCode* code) {
  bool eligible = true;
  uint32_t count = 0;
  editable_cfg_adapter::iterate(code, [&](const MethodItemEntry& mie) {
    auto opcode = mie.insn->opcode();
    if (opcode::is_invoke_super(opcode)) {
      eligible = false;
      return editable_cfg_adapter::LOOP_BREAK;
    }
    if (!opcode::is_an_internal(opcode)) {
      count++;
    }
    if (count > MAX_NUM_INSTRUCTIONS) {
      eligible = false;
      return editable_cfg_adapter::LOOP_BREAK;
    }
    return editable_cfg_adapter::LOOP_CONTINUE;
  });
  return eligible;
}

void find_duplications(const method_override_graph::Graph* graph,
                       const DexMethod* root_method,
                       std::vector<DexMethod*>* result) {
  auto root_code = root_method->get_code();
  if (!root_code) {
    return;
  }
  for (auto* child : graph->get_node(root_method).children) {
    // The method definition may be deleted after the overriding graph is
    // created, check if it's still a definition.
    if (root(child) || !child->is_def() || !can_rename(child)) {
      continue;
    }
    auto child_code = child->get_code();
    if (child_code && eligible_code(child_code) &&
        root_code->structural_equals(*child_code)) {
      result->push_back(const_cast<DexMethod*>(child));
      find_duplications(graph, child, result);
    }
  }
}

/**
 * Make the method and all its overriding methods be public.
 */
void publicize_methods(const method_override_graph::Graph* graph,
                       DexMethod* root_method) {
  set_public(root_method);
  for (auto* child : graph->get_node(root_method).children) {
    if (is_public(child)) {
      // The children of child should all be public, otherwise the code is
      // invalid before this transformation.
      continue;
    }
    redex_assert(is_protected(child));
    publicize_methods(graph, const_cast<DexMethod*>(child));
  }
}

/**
 * Deduplicate identical overriding code.
 */
uint32_t remove_duplicated_vmethods(const Scope& scope) {
  uint32_t ret = 0;
  auto graph = method_override_graph::build_graph(scope);
  std::unordered_map<DexMethodRef*, DexMethodRef*> removed_vmethods;

  walk::classes(scope, [&](DexClass* cls) {
    for (auto method : cls->get_vmethods()) {
      if (!method->get_code()) {
        // TODO: look at the abstract methods and we can lift the
        // implementations to the abstract class if lots of them are identical.
        continue;
      }
      if (!is_public_or_protected(method)) {
        // Note: package-private methods are skipped. Need consider package
        // names when change the accessibility of them.
        continue;
      }
      if (!eligible_code(method->get_code())) {
        continue;
      }
      std::vector<DexMethod*> duplicates;
      find_duplications(graph.get(), method, &duplicates);
      if (!duplicates.empty()) {
        if (is_protected(method)) {
          publicize_methods(graph.get(), method);
        }
        TRACE(VM, 8, "Same as %s", SHOW(method));
        for (auto m : duplicates) {
          TRACE(VM, 8, "\t%s", SHOW(m));
          type_class(m->get_class())->remove_method(m);
          removed_vmethods.emplace(m, method);
          DexMethod::erase_method(m);
          DexMethod::delete_method(m);
        }
        ret += duplicates.size();
        TRACE(VM, 9, "%s\n", SHOW(method->get_code()));
      }
    }
  });

  method_dedup::fixup_references_to_removed_methods(scope, removed_vmethods);

  return ret;
}
} // namespace

namespace dedup_vmethods {

uint32_t dedup(const DexStoresVector& stores) {
  auto scope = build_class_scope(stores);
  auto deduplicated_vmethods = remove_duplicated_vmethods(scope);
  TRACE(VM, 2, "deduplicated_vmethods %d\n", deduplicated_vmethods);
  return deduplicated_vmethods;
}

} // namespace dedup_vmethods
