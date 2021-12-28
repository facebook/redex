/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAccess.h"
#include "DexClass.h"
#include "MethodOverrideGraph.h"
#include "Show.h"
#include "Walkers.h"

namespace {
void overriden_should_not_be_public(
    const DexMethod* method,
    const method_override_graph::Graph* graph,
    std::unordered_set<const DexMethod*>* should_not_mark) {
  if (method->is_external()) {
    return;
  }
  should_not_mark->insert(method);
  const auto& node = graph->get_node(method);
  for (const auto* overriden : node.parents) {
    overriden_should_not_be_public(overriden, graph, should_not_mark);
  }
}
/**
 * XXX: Why not simply mark the virtual methods public? For a virtual method,
 * there can be an invisible final virtual method with the same signature in the
 * parents, they do not have overriding relationship. Change the latter method
 * be visible to it is wrong.
 */
void loosen_access_modifier_for_vmethods(const DexClasses& scope) {
  auto graph = method_override_graph::build_graph(scope);
  const auto& nodes = graph->nodes();
  std::unordered_set<const DexMethod*> should_not_mark;
  for (const auto& pair : nodes) {
    const auto* method = pair.first;
    // If a final method has children, it can only be package-private and we can
    // not change it to be public.
    if (is_final(method) && !pair.second.children.empty()) {
      overriden_should_not_be_public(method, graph.get(), &should_not_mark);
      always_assert_log(!is_public(method) && !is_protected(method),
                        "%s is visible final but it has children",
                        SHOW(method));
    }
  }
  walk::parallel::classes(scope, [&should_not_mark](DexClass* cls) {
    for (auto* method : cls->get_vmethods()) {
      if (!should_not_mark.count(method)) {
        set_public(method);
      }
    }
  });
}
} // namespace

void loosen_access_modifier_except_vmethods(DexClass* clazz) {
  set_public(clazz);
  for (auto field : clazz->get_ifields()) {
    set_public(field);
  }
  for (auto field : clazz->get_sfields()) {
    set_public(field);
  }
  // Direct methods should have one of the modifiers, ACC_STATIC, ACC_PRIVATE
  // or ACC_CONSTRUCTOR.
  for (auto method : clazz->get_dmethods()) {
    auto access = method->get_access();
    if (access & (ACC_STATIC | ACC_CONSTRUCTOR)) {
      set_public(method);
    }
  }
}

void loosen_access_modifier(const DexClasses& classes) {
  walk::parallel::classes(classes, [](DexClass* clazz) {
    loosen_access_modifier_except_vmethods(clazz);
  });
  loosen_access_modifier_for_vmethods(classes);

  DexType* dalvikinner = DexType::get_type("Ldalvik/annotation/InnerClass;");
  if (!dalvikinner) {
    return;
  }

  walk::annotations(classes, [&dalvikinner](DexAnnotation* anno) {
    if (anno->type() != dalvikinner) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      // Fix access flags on all @InnerClass annotations
      if (!strcmp("accessFlags", elem.string->c_str())) {
        always_assert(elem.encoded_value->evtype() == DEVT_INT);
        elem.encoded_value->value(
            (elem.encoded_value->value() & ~VISIBILITY_MASK) | ACC_PUBLIC);
      }
    }
  });
}
