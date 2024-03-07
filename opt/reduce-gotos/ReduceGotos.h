/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

class ReduceGotosPass : public Pass {
 public:
  struct Stats {
    size_t removed_switches{0};
    size_t reduced_switches{0};
    size_t replaced_trivial_switches{0};
    size_t remaining_trivial_switches{0};
    size_t remaining_two_case_switches{0};
    size_t remaining_range_switches{0};
    size_t remaining_range_switch_cases{0};
    size_t removed_switch_cases{0};
    size_t replaced_gotos_with_returns{0};
    size_t removed_trailing_moves{0};
    size_t inverted_conditional_branches{0};
    size_t replaced_gotos_with_throws{0};

    Stats& operator+=(const Stats&);
  };

  ReduceGotosPass() : Pass("ReduceGotosPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoInitClassInstructions, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoUnreachableInstructions, Preserves},
        {RenameClass, Preserves},
    };
  }
  bool is_cfg_legacy() override { return true; }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static Stats process_code(IRCode*);
  static void process_code_switches(cfg::ControlFlowGraph&, Stats&);
  static void process_code_ifs(cfg::ControlFlowGraph&, Stats&);

 private:
  static void shift_registers(cfg::ControlFlowGraph* cfg, uint32_t* reg);
};
