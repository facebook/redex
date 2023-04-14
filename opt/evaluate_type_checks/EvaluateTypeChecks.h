/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Shrinker.h"

#include <boost/optional.hpp>

class DexType;
class DexMethod;
class XStoreRefs;

namespace check_casts {

class EvaluateTypeChecksPass : public Pass {
 public:
  EvaluateTypeChecksPass() : Pass("EvaluateTypeChecksPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}}};
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // Exposed for testing.
  static boost::optional<int32_t> evaluate(const DexType* src_type,
                                           const DexType* test_type);
  static void optimize(DexMethod* m, shrinker::Shrinker& shrinker);
};

} // namespace check_casts
