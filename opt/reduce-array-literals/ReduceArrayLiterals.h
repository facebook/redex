/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "IRInstruction.h" // For reg_t.
#include "Pass.h"
#include "RedexOptions.h" // For Architecture.

class DexType;

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

class ReduceArrayLiterals {
 public:
  struct Stats {
    size_t filled_arrays{0};
    size_t filled_array_chunks{0};
    size_t filled_array_elements{0};
    size_t remaining_wide_arrays{0};
    size_t remaining_wide_array_elements{0};
    size_t remaining_unimplemented_arrays{0};
    size_t remaining_unimplemented_array_elements{0};
    size_t remaining_buggy_arrays{0};
    size_t remaining_buggy_array_elements{0};

    Stats& operator+=(const Stats&);
  };

  ReduceArrayLiterals(cfg::ControlFlowGraph&,
                      size_t max_filled_elements,
                      int32_t min_sdk,
                      Architecture arch);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  void patch();

 private:
  void patch_new_array(const IRInstruction* new_array_insn,
                       const std::vector<const IRInstruction*>& aput_insns);
  size_t patch_new_array_chunk(
      DexType* type,
      size_t chunk_start,
      const std::vector<const IRInstruction*>& aput_insns,
      boost::optional<reg_t> chunk_dest,
      reg_t overall_dest,
      std::vector<reg_t>* temp_regs);
  cfg::ControlFlowGraph& m_cfg;
  size_t m_max_filled_elements;
  int32_t m_min_sdk;
  std::vector<reg_t> m_local_temp_regs;
  Stats m_stats;
  std::vector<
      std::pair<const IRInstruction*, std::vector<const IRInstruction*>>>
      m_array_literals;
  Architecture m_arch;
};

class ReduceArrayLiteralsPass : public Pass {
 public:
  ReduceArrayLiteralsPass() : Pass("ReduceArrayLiteralsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}}};
  }

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_max_filled_elements;
  bool m_debug;
};
