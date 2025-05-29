/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAccess.h"

#include "DeterministicContainers.h"
#include "DexClass.h"
#include "MethodOverrideGraph.h"
#include "Show.h"
#include "Walkers.h"

namespace {
void overriden_should_not_be_public(
    const method_override_graph::Node* method,
    const method_override_graph::Graph* graph,
    UnorderedSet<const DexMethod*>* should_not_mark) {
  if (method->method->is_external()) {
    return;
  }
  should_not_mark->insert(method->method);
  for (const auto* overriden : UnorderedIterable(method->parents)) {
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
  UnorderedSet<const DexMethod*> should_not_mark;
  for (const auto& pair : UnorderedIterable(nodes)) {
    const auto* method = pair.first;
    // If a final method has children, it can only be package-private and we can
    // not change it to be public.
    if (is_final(method) && !pair.second.children.empty()) {
      overriden_should_not_be_public(
          &pair.second, graph.get(), &should_not_mark);
      auto& children = pair.second.children;
      auto* first_child = *unordered_any(children);
      always_assert_log(!is_public(method) && !is_protected(method),
                        "%s is visible final but it has children %s",
                        SHOW(method->get_deobfuscated_name()),
                        SHOW(first_child->method->get_deobfuscated_name()));
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
    auto& elems = anno->anno_elems();
    for (auto& elem : elems) {
      // Fix access flags on all @InnerClass annotations
      if (!strcmp("accessFlags", elem.string->c_str())) {
        always_assert(elem.encoded_value->evtype() == DEVT_INT);
        elem.encoded_value->value(
            (elem.encoded_value->value() & ~VISIBILITY_MASK) | ACC_PUBLIC);
      }
    }
  });
}
