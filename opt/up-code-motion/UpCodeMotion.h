/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "Pass.h"
#include "Trace.h"

class UpCodeMotionPass : public Pass {
 public:
  struct Stats {
    size_t instructions_moved{0};
    size_t branches_moved_over{0};
    size_t inverted_conditional_branches{0};
    size_t clobbered_registers{0};
    size_t skipped_branches{0};

    Stats& operator+=(const Stats& that) {
      instructions_moved += that.instructions_moved;
      branches_moved_over += that.branches_moved_over;
      inverted_conditional_branches += that.inverted_conditional_branches;
      clobbered_registers += that.clobbered_registers;
      skipped_branches += that.skipped_branches;
      return *this;
    }
  };

  UpCodeMotionPass() : Pass("UpCodeMotionPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, RequiresAndEstablishes},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void bind_config() override {
    bind("check_branch_hotness",
         false,
         m_check_if_branch_is_hot,
         "Don't move branch target blocks that are not hot");
  }

  static Stats process_code(bool is_static,
                            DexType* declaring_type,
                            DexTypeList* args,
                            IRCode*,
                            bool is_branch_hot_check);

 private:
  bool m_check_if_branch_is_hot;
  static bool gather_movable_instructions(
      cfg::Block* b, std::vector<IRInstruction*>* instructions);
  static bool gather_instructions_to_insert(
      cfg::Edge* branch_edge,
      cfg::Edge* goto_edge,
      std::vector<IRInstruction*>* instructions_to_insert);
  static bool is_hot(cfg::Block* b);
};
