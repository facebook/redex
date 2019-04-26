/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

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
  };

  ReduceArrayLiterals(cfg::ControlFlowGraph&,
                      size_t max_filled_elements,
                      int32_t min_sdk);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  void patch();

 private:
  void patch_new_array(IRInstruction* new_array_insn);
  size_t patch_new_array_chunk(DexType* type,
                               size_t chunk_start,
                               const std::vector<IRInstruction*>& aput_insns,
                               boost::optional<uint16_t> chunk_dest,
                               uint16_t overall_dest,
                               std::vector<uint16_t>* temp_regs);
  cfg::ControlFlowGraph& m_cfg;
  size_t m_max_filled_elements;
  int32_t m_min_sdk;
  std::vector<uint16_t> m_local_temp_regs;
  Stats m_stats;
  std::unordered_map<IRInstruction*, std::vector<IRInstruction*>>
      m_array_literals;
};

class ReduceArrayLiteralsPass : public Pass {
 public:
  ReduceArrayLiteralsPass() : Pass("ReduceArrayLiteralsPass") {}

  void configure_pass(const JsonWrapper& jw) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_max_filled_elements;
  bool m_debug;
};
