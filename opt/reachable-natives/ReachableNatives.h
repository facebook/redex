/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "ConstantUses.h"
#include "IRList.h"
#include "Pass.h"
#include "TypeInference.h"

/*
   This pass will identify native methods which are (un)reachable, ignoring
   "blanket native" proguard rules which keep all native methods and classes
   with native methods, e.g.

   -keepclasseswithmembers class * {
      native <methods>;
    }

    It just runs reachability analysis in the same way as RMU does, but does
    not mark "blanket native" classes/methods as roots.  Classes and methods
    which are kept only due to a "blanket native" rule have been identified
    during proguard processing and stored in RedexContext.

    Results are written to a file, named "redex-reachable-natives.txt" by
    default, and stats on the number of (un)reachable native methods are
    logged.

*/

class ReachableNativesPass : public Pass {
 public:
  ReachableNativesPass() : Pass("ReachableNativesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_output_file_name;
  size_t m_run_number = 0;
};
