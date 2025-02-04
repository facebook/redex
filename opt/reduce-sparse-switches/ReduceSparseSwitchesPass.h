/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <limits>

#include "Pass.h"
#include "PassManager.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

class ReduceSparseSwitchesPass : public Pass {
 public:
  struct Stats {
    size_t affected_methods{0};

    size_t splitting_transformations{0};
    size_t splitting_transformations_packed_segments{0};
    size_t splitting_transformations_switch_cases_packed{0};

    struct Multiplexing {
      size_t abandoned{0};
      size_t transformations{0};
      size_t switch_cases{0};
      size_t inefficiency{0};

      Multiplexing& operator+=(const Multiplexing&);
    };
    std::unordered_map<size_t, Multiplexing> multiplexing;

    size_t multiplexing_transformations() const;

    size_t multiplexing_switch_cases() const;

    Stats& operator+=(const Stats&);
  };

  struct Config {
    // Starting at 10, the splitting transformation is always a code-size win
    uint64_t min_splitting_switch_cases{10};

    uint64_t min_multiplexing_switch_cases{64};

    uint64_t write_sparse_switches{std::numeric_limits<uint64_t>::max()};
  };

  ReduceSparseSwitchesPass() : Pass("ReduceSparseSwitchesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
This pass reduces sparse switch instructions.

Sparse switches are expensive at runtime when they get compiled by ART, as 
they get translated to a linear sequence of conditional branches which take
O(N) time to execute, where N is the number of switch cases.

This pass performs two transformations which are designed to improve
runtime performance:

1. Splitting sparse switches into a main packed switch and a secondary sparse
   switch. This transformation is only performed if we find a packed
   sub-sequence of case keys that contains at least half of all case keys,
   so that we shave off at least one operation from the binary search the
   interpreter needs to do over the remaining sparse case keys, making sure
   we never degrade worst-case complexity. We run this to a fixed point.
2. Multiplexing sparse switches into a main packed switch with secondary sparse
   switches for each main switch case. The basic idea is that we partition a 
   large number of sparse switch cases into several buckets of relatively small 
   sparse switch cases. The bucket index is basically a hash of the case keys, 
   computed with one or two bit-twiddling instructions, and limited to a small 
   numeric range, which allows us to perform a packed switch over it. Ideally, 
   each bucket holds roughly the same number of switch cases, and we want to 
   avoid excessively large outlier buckets.
   This transformation comes with a modest size regression. 
   Given a switch with N case keys, we aim at partitioning it into 
   M = ~sqrt(N) buckets with ~sqrt(N) case keys in each bucket. (We don't 
   achieve that in practice, and there are rounding effects as well.)
   In that case, before the transformation, the interpreter would have O(log N)
   and compiled code O(N). After the tranformation, the interpreter gets down 
   to O(log sqrt(N)) and compiled code to O(sqrt(N)).
   (We could try to partition buckets even further, e.g. down to log(N), but 
   that might result in an excessive size regression.)
    )");
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static ReduceSparseSwitchesPass::Stats splitting_transformation(
      size_t min_switch_cases, cfg::ControlFlowGraph& cfg);

  static ReduceSparseSwitchesPass::Stats multiplexing_transformation(
      size_t min_switch_cases, cfg::ControlFlowGraph& cfg);

 private:
  Config m_config;
};
