/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class DedupBlocksPass : public Pass {
 public:
  DedupBlocksPass() : Pass("DedupBlocksPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void bind_config() override {
    bind("method_black_list", {}, m_config.method_black_list);
    bind("block_split_min_opcode_count",
         Config::DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT,
         m_config.block_split_min_opcode_count);
    bind("split_postfix", true, m_config.split_postfix);
    bind("debug", false, m_config.debug);
  }

  struct Config {
    std::unordered_set<DexMethod*> method_black_list;
    static const unsigned int DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT = 2;
    unsigned int block_split_min_opcode_count =
        DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT;
    bool split_postfix = true;
    bool debug = false;
  } m_config;
};
