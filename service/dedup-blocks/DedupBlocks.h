/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace dedup_blocks_impl {

struct Config {
  std::unordered_set<DexMethod*> method_black_list;
  static const unsigned int DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT = 2;
  unsigned int block_split_min_opcode_count =
      DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT;
  bool split_postfix = true;
  bool debug = false;
};

struct Stats {
  int eligible_blocks{0};
  int blocks_removed{0};
  int blocks_split{0};
  // map from block size to number of blocks with that size
  std::unordered_map<size_t, size_t> dup_sizes;
  Stats& operator+=(const Stats& that);
};

class DedupBlocks {
 public:
  DedupBlocks(const Config& config, DexMethod* method);

  const Stats& get_stats() const { return m_stats; }

  void run();

 private:
  const Config& m_config;
  DexMethod* m_method;
  Stats m_stats;
};

} // namespace dedup_blocks_impl
