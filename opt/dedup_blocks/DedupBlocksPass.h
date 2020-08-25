/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void bind_config() override {
    bind("method_blocklist", {}, m_config.method_blocklist);
    bind("block_split_min_opcode_count",
         dedup_blocks_impl::Config::DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT,
         m_config.block_split_min_opcode_count);
    bind("split_postfix", true, m_config.split_postfix);
    bind("debug", false, m_config.debug);
  }

 private:
  void report_stats(PassManager& mgr, const dedup_blocks_impl::Stats& stats);
  dedup_blocks_impl::Config m_config;
};
