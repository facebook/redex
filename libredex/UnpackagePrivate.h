/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Walkers.h"

inline void unpackage_private(Scope& scope) {
  walk::methods(scope, [&](DexMethod* method) {
    if (is_package_protected(method)) set_public(method);
  });
  walk::fields(scope, [&](DexField* field) {
    if (is_package_protected(field)) set_public(field);
  });
  for (auto clazz : scope) {
    if (!clazz->is_external()) {
      set_public(clazz);
    }
  }

  static DexType* dalvikinner =
      DexType::get_type("Ldalvik/annotation/InnerClass;");

  walk::annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalvikinner) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      // Fix access flags on all @InnerClass annotations
      if (!strcmp("accessFlags", elem.string->c_str())) {
        always_assert(elem.encoded_value->evtype() == DEVT_INT);
        elem.encoded_value->value(
            (elem.encoded_value->value() & ~VISIBILITY_MASK) | ACC_PUBLIC);
        TRACE(RENAME, 3, "Fix InnerClass accessFlags %s => %08x",
              elem.string->c_str(), elem.encoded_value->value());
      }
    }
  });
}
