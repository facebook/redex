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
    size_t filled_array_elements{0};
    size_t remaining_wide_arrays{0};
    size_t remaining_wide_array_elements{0};
    size_t remaining_large_arrays{0};
    size_t remaining_large_array_elements{0};
    size_t remaining_unimplemented_arrays{0};
    size_t remaining_unimplemented_array_elements{0};
    size_t remaining_buggy_arrays{0};
    size_t remaining_buggy_array_elements{0};
  };

  ReduceArrayLiterals(cfg::ControlFlowGraph&);

  const Stats& get_stats() const { return m_stats; }

  /*
   * Patch code based on analysis results.
   */
  void patch(cfg::ControlFlowGraph&,
             size_t max_filled_elements,
             int32_t min_sdk);

 private:
  Stats m_stats;
  std::unordered_map<IRInstruction*, std::vector<IRInstruction*>>
      m_array_literals;
};

class ReduceArrayLiteralsPass : public Pass {
 public:
  ReduceArrayLiteralsPass() : Pass("ReduceArrayLiteralsPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("debug", false, m_debug);
    jw.get("max_filled_elements", 222, m_max_filled_elements);
    always_assert(m_max_filled_elements < 0xff);
  }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_max_filled_elements;
  bool m_debug;
};
