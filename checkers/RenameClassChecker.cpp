/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RenameClassChecker.h"

#include "Debug.h"
#include "DexClass.h"
#include "Show.h"
#include "Walkers.h"

#include <locator.h>
using facebook::Locator;

namespace redex_properties {

void RenameClassChecker::run_checker(DexStoresVector& stores,
                                     ConfigFiles& /* conf */,
                                     PassManager& /* mgr */,
                                     bool established) {
  if (!established) {
    return;
  }
  const auto& scope = build_class_scope(stores);
  uint32_t sequence_nr = 0;
  walk::classes(scope, [&](const DexClass* cls) {
    const char* cls_name = cls->get_name()->c_str();
    uint32_t global_cls_nr = Locator::decodeGlobalClassIndex(cls_name);
    if (global_cls_nr != Locator::invalid_global_class_index) {
      always_assert_log(
          sequence_nr == global_cls_nr,
          "[%s] invalid class number, expected %u, got %u, class %s!\n",
          get_name(get_property()), sequence_nr, global_cls_nr, cls_name);
    }
    ++sequence_nr;
  });
}

} // namespace redex_properties

namespace {
static redex_properties::RenameClassChecker s_checker;
} // namespace
