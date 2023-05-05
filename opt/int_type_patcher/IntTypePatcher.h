/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "Pass.h"
#include "TypeInference.h"

class IntTypePatcherPass : public Pass {
 public:
  IntTypePatcherPass() : Pass("IntTypePatcherPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {{DexLimitsObeyed, Preserves},
            {HasSourceBlocks, Preserves},
            {UltralightCodePatterns, Preserves},
            {NoInitClassInstructions, Preserves},
            {NeedsEverythingPublic, Preserves},
            {NeedsInjectionIdLowering, Preserves},
            {NoSpuriousGetClassCalls, Preserves},
            {RenameClass, Preserves}};
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;
  void run(DexMethod* m);

  void convert_to_boolean(cfg::ControlFlowGraph& cfg,
                          cfg::Block* exit_block,
                          IRInstruction* insn);

  void convert_int_to(IROpcode opcode,
                      cfg::ControlFlowGraph& cfg,
                      cfg::Block* exit_block,
                      IRInstruction* insn);

  bool return_type_mismatch(const type_inference::IntTypeDomain& int_type,
                            const type_inference::IntTypeDomain& return_type);

  bool is_editable_cfg_friendly() override { return true; }

 private:
  ConcurrentSet<DexMethod*> changed_methods;
  std::atomic<size_t> added_insns{0};
  std::atomic<size_t> mismatched_bool{0};
  std::atomic<size_t> mismatched_byte{0};
  std::atomic<size_t> mismatched_char{0};
  std::atomic<size_t> mismatched_short{0};
};
