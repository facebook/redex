/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class DexFieldRef;
class IRCode;

// A pass to remove redundant (based on MinSDKVersion) API level checks.
// TODO: Support ranges in ConstantPropagation instead of this one-off pass.
class RemoveApiLevelChecksPass : public Pass {
 public:
  RemoveApiLevelChecksPass() : Pass("RemoveApiLevelChecksPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}}};
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  struct ApiLevelStats {
    size_t num_field_gets = 0;
    size_t num_removed = 0;
    size_t num_methods = 0;

    ApiLevelStats() {}
    ApiLevelStats(size_t gets, size_t removed, size_t methods)
        : num_field_gets(gets), num_removed(removed), num_methods(methods) {}

    ApiLevelStats& operator+=(const ApiLevelStats& rhs) {
      num_field_gets += rhs.num_field_gets;
      num_removed += rhs.num_removed;
      num_methods += rhs.num_methods;
      return *this;
    }
  };

  static const DexFieldRef* get_sdk_int_field();
  static ApiLevelStats run(IRCode* code,
                           int32_t min_sdk,
                           const DexFieldRef* sdk_int_field);

  friend class RemoveApiLevelChecksTest;
};
