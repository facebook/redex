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

class InsertSourceBlocksPass : public Pass {
 public:
  InsertSourceBlocksPass() : Pass("InsertSourceBlocksPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Establishes},
        {UltralightCodePatterns, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
A pass to insert SourceBlock MIEs into CFGs.

This is a pass so it can be more freely scheduled. A simple example is
to run this *after* the first RemoveUnreachables pass, so as to not
create unnecessary bloat.
    )");
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_profile_files;
  bool m_force_serialize{false};
  bool m_force_run{false};
  bool m_insert_after_excs{true};
  bool m_always_inject{true};
  bool m_use_default_value{false};
  bool m_fix_violations{false};
  std::vector<std::string> m_ordered_interactions;

  friend class SourceBlocksTest;
  friend class SourceBlocksDedupTest;
};
