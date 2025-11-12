/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/**
 * Change sget instructions on known resource class fields to a special IR
 * opcode with the field's encoded value as a literal. This is to differentiate
 * a resource constant with any other unrelated constant that happens to be in
 * the resource ID range.
 */
class MaterializeResourceConstantsPass : public Pass {
 public:
  MaterializeResourceConstantsPass()
      : Pass("MaterializeResourceConstantsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
A pass that replaces all instructions of the form `sget vx, Lsome/path/R$sometype;.SomeResourceId:I`
with `R_CONST vx, #I` where #I is the literal value of that ID inlined into the instruction. This helps
with dead resource tracking as Redex now tracks which instructions point to resource IDs.

Note that this pass also simplifies the clinit of all R$ classes to resolve the static values of their fields.
    )");
  }

  void bind_config() override {
    bind("replace_const_instructions", false, m_replace_const_instructions,
         "Whether or not to replace regular sget instructions with an R_CONST "
         "opcode.");
  }
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override {}
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_replace_const_instructions{false};
};
