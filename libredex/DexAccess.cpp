/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAccess.h"
#include "DexClass.h"
#include "Walkers.h"

void loosen_access_modifier(DexClass* clazz) {
  set_public(clazz);
  for (auto field : clazz->get_ifields()) {
    set_public(field);
  }
  for (auto field : clazz->get_sfields()) {
    set_public(field);
  }
  for (auto method : clazz->get_vmethods()) {
    set_public(method);
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

void loosen_access_modifier(const Scope& scope) {
  walk::parallel::classes(
      scope, [](DexClass* clazz) { loosen_access_modifier(clazz); });

  DexType* dalvikinner = DexType::get_type("Ldalvik/annotation/InnerClass;");
  if (!dalvikinner) {
    return;
  }

  walk::annotations(scope, [&dalvikinner](DexAnnotation* anno) {
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
