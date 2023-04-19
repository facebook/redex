/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DedupBlocks.h"
#include "Pass.h"

class DedupBlocksPass : public Pass {
 public:
  DedupBlocksPass() : Pass("DedupBlocksPass") {}

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
    bind("method_blocklist", {}, m_config.method_blocklist);
    bind("block_split_min_opcode_count",
         dedup_blocks_impl::Config::DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT,
         m_config.block_split_min_opcode_count);
    bind("split_postfix", true, m_config.split_postfix);
    bind("debug", false, m_config.debug);
    bind(
        "dedup_fill_in_stack_trace", false, m_config.dedup_fill_in_stack_trace);
    bind("max_iteration", 10, m_config.max_iteration);
  }

 private:
  void report_stats(PassManager& mgr, const dedup_blocks_impl::Stats& stats);
  dedup_blocks_impl::Config m_config;
};
