/**
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

  void configure_pass(const JsonWrapper& jw) override {
    std::vector<std::string> method_black_list_names;
    jw.get("method_black_list", {}, method_black_list_names);
    for (std::string name : method_black_list_names) {
      auto meth = DexMethod::get_method(name);
      if (meth == nullptr || !meth->is_def()) continue;
      m_config.method_black_list.emplace(static_cast<DexMethod*>(meth));
    }
    jw.get("block_split_min_opcode_count",
           Config::DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT,
           m_config.block_split_min_opcode_count);
    jw.get("split_postfix", true, m_config.split_postfix);
  }

  struct Config {
    std::unordered_set<DexMethod*> method_black_list;
    static const size_t DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT = 3;
    size_t block_split_min_opcode_count = DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT;
    bool split_postfix = true;
  } m_config;
};
