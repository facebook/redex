/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace dedup_blocks_impl {

struct Config {
  std::unordered_set<DexMethod*> method_blocklist;
  static const unsigned int DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT = 1;
  unsigned int block_split_min_opcode_count =
      DEFAULT_BLOCK_SPLIT_MIN_OPCODE_COUNT;
  bool split_postfix = true;
  bool debug = false;
  bool dedup_throws = false;
};

struct Stats {
  int eligible_blocks{0};
  int blocks_removed{0};
  int insns_removed{0};
  int blocks_split{0};
  int positions_inserted{0};
  // map from block size to number of blocks with that size
  std::unordered_map<size_t, size_t> dup_sizes;
  Stats& operator+=(const Stats& that);
};

class DedupBlocks {
 public:
  DedupBlocks(const Config* config, DexMethod* method);
  DedupBlocks(const Config* config,
              IRCode* code,
              bool is_static,
              DexType* declaring_type,
              DexTypeList* args);

  const Stats& get_stats() const { return m_stats; }

  void run();

 private:
  const Config* m_config;
  IRCode* m_code;
  bool m_is_static;
  DexType* m_declaring_type;
  DexTypeList* m_args;
  Stats m_stats;
};

} // namespace dedup_blocks_impl
