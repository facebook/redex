/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MakePublicPass.h"

#include "DexUtil.h"
#include "Walkers.h"

namespace {
void public_everything(const Scope& scope) {
  walk::fields(scope, [](DexField* field) { set_public(field); });
  walk::parallel::classes(scope, [](DexClass* clazz) {
    set_public(clazz);
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
  });

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
} // namespace

void MakePublicPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& mgr) {
  auto scope = build_class_scope(stores);
  public_everything(scope);
}

static MakePublicPass s_pass;
