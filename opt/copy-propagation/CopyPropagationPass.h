/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CopyPropagation.h"
#include "Pass.h"

class CopyPropagationPass : public Pass {
 public:
  CopyPropagationPass() : Pass("CopyPropagationPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {
        {NoInitClassInstructions, {.preserves = true}},
        {HasSourceBlocks, {.preserves = true}},
        {NoSpuriousGetClassCalls, {.preserves = true}},
        {RenameClass, {.preserves = true}},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void bind_config() override {
    // This option can only be safely enabled in verify-none. `run_pass` will
    // override this value to false if we aren't in verify-none. Here's why:
    //
    // const v0, 0
    // sput v0, someFloat   # uses v0 as a float
    // const v0, 0          # This could be eliminated (in verify-none)
    // sput v0, someInt     # uses v0 as an int
    //
    // The android verifier insists on having the second const load because
    // using v0 as a float gives it type float. But, in reality the bits in the
    // register are the same, so in verify none mode, we can eliminate the
    // second const load
    //
    // TODO: detect the type of constant for each alias group
    bind("eliminate_const_literals", false, m_config.eliminate_const_literals);
    bind("eliminate_const_literals_with_same_type_demands", true,
         m_config.eliminate_const_literals_with_same_type_demands);
    bind("eliminate_const_strings", true, m_config.eliminate_const_strings);
    bind("eliminate_const_classes", true, m_config.eliminate_const_classes);
    bind("replace_with_representative", true,
         m_config.replace_with_representative);
    bind("wide_registers", true, m_config.wide_registers);
    bind("static_finals", true, m_config.static_finals);
    bind("debug", false, m_config.debug);
  }

  copy_propagation_impl::Config m_config;
};
