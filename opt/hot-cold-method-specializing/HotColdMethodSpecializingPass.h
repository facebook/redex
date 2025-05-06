/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Shrinker.h"

class HotColdMethodSpecializingPass : public Pass {
 public:
  struct Stats {
    size_t methods_with_efficient_cold_frontier{0};
    size_t methods_with_inefficient_cold_frontier{0};
    size_t unspecializable_cold_code{0};
    size_t proposed_cold_frontier_blocks{0};
    size_t pruned_cold_frontier_blocks{0};
    size_t selected_cold_frontier_blocks{0};
    uint64_t original_code_units{0};
    uint64_t hot_code_units{0};
    uint64_t cold_code_units{0};
    bool empty() const {
      return methods_with_efficient_cold_frontier +
                     methods_with_inefficient_cold_frontier ==
                 0 &&
             unspecializable_cold_code == 0 &&
             proposed_cold_frontier_blocks + pruned_cold_frontier_blocks +
                     selected_cold_frontier_blocks ==
                 0 &&
             original_code_units == 0;
    }
    Stats& operator+=(const Stats& other) {
      methods_with_efficient_cold_frontier +=
          other.methods_with_efficient_cold_frontier;
      methods_with_inefficient_cold_frontier +=
          other.methods_with_inefficient_cold_frontier;
      unspecializable_cold_code += other.unspecializable_cold_code;
      proposed_cold_frontier_blocks += other.proposed_cold_frontier_blocks;
      pruned_cold_frontier_blocks += other.pruned_cold_frontier_blocks;
      selected_cold_frontier_blocks += other.selected_cold_frontier_blocks;
      original_code_units += other.original_code_units;
      hot_code_units += other.hot_code_units;
      cold_code_units += other.cold_code_units;
      return *this;
    }
  };

  struct Config {
    float threshold_factor{1.667};
    float threshold_offset{16};
    std::vector<std::string> blocklist;
  };

  HotColdMethodSpecializingPass() : Pass("HotColdMethodSpecializingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, RequiresAndEstablishes},
        {NoResolvablePureRefs, Preserves},
        {SpuriousGetClassCallsInterned, Preserves},
        {InitialRenameClass, Preserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
This optimization pass identifies methods with a significant pure hot prefix
followed by cold code. It then splits these methods into two separate methods:
a hot method and a cold method.
The resulting hot method includes calls to the newly created cold method,
replacing the original conditional transitions from hot to cold blocks.
The split-out code retains the hot prefix, which will be executed twice at
runtime. To optimize further, any unreachable code in the cold method is
replaced with an "unreachable" instruction, reducing the code size regression.
The now smaller hot method may be inlined into any callers.
This pass is inspired by partial inlining, which also has a notion of a pure
hot prefix, which also makes it different from the MethodSplittingPass, which
will never duplicate leading basic blocks.
    )");
  }

  void bind_config() override;

  void run_pass(DexStoresVector& stores,
                ConfigFiles& config,
                PassManager& mgr) override;

  static Stats analyze_and_specialize(const Config& config,
                                      size_t iteration,
                                      DexMethod* method,
                                      DexMethod** cold_copy,
                                      shrinker::Shrinker* shrinker = nullptr);

 private:
  size_t m_iteration{0};
  Config m_config;
};
