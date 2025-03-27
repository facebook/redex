/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RenameClassChecker.h"

#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "Show.h"
#include "Walkers.h"

#include <locator.h>
using facebook::Locator;

namespace redex_properties {

void RenameClassChecker::run_checker(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& /* mgr */,
                                     bool established) {
  if (!established) {
    return;
  }

  int64_t max_sequence_nr_in_prev_stores = -1;
  for (auto& store : stores) {
    auto& dexen = store.get_dexen();
    int64_t max_sequence_nr_in_store = -1;
    for (auto& dex : dexen) {
      bool first_in_dex = true;
      std::optional<uint32_t> last_sequence_nr_in_dex;
      for (auto* cls : dex) {
        const char* cls_name = cls->get_name()->c_str();
        uint32_t global_cls_nr = Locator::decodeGlobalClassIndex(cls_name);
        if (global_cls_nr != Locator::invalid_global_class_index) {
          // All sequence numbers within a store must be larger then sequence
          // numbers in previous stores.
          if (first_in_dex) {
            always_assert_log(
                max_sequence_nr_in_prev_stores < (int64_t)global_cls_nr,
                "[%s] invalid class number, expected a number larger than %u, "
                "got %u, class %s!\n",
                get_name(get_property()),
                (uint32_t)max_sequence_nr_in_prev_stores, global_cls_nr,
                cls_name);
            first_in_dex = false;
          }

          // No matter what, sequence numbers within a dex must be increasing
          if (last_sequence_nr_in_dex) {
            always_assert_log(*last_sequence_nr_in_dex < global_cls_nr,
                              "[%s] invalid class number, expected a number "
                              "larger than %u, got %u, class %s!\n",
                              get_name(get_property()),
                              *last_sequence_nr_in_dex, global_cls_nr,
                              cls_name);
          }
          last_sequence_nr_in_dex = global_cls_nr;
          max_sequence_nr_in_store =
              std::max(max_sequence_nr_in_store, (int64_t)global_cls_nr);
        }
      }
    }
    max_sequence_nr_in_prev_stores =
        std::max(max_sequence_nr_in_prev_stores, max_sequence_nr_in_store);
  }
}

} // namespace redex_properties

namespace {
static redex_properties::RenameClassChecker s_checker;
} // namespace
