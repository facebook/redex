/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <sstream>

#include "DexClass.h"

namespace source_blocks {

using namespace cfg;

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  bool serialize) {
  uint32_t id{0};
  std::ostringstream oss;

  auto block_start_fn = [&](Block* cur) {
    if (serialize) {
      oss << "(" << id;
    }

    source_blocks::impl::BlockAccessor::push_source_block(
        cur, std::make_unique<SourceBlock>(method, id, 0.0f));
    ++id;
  };
  auto edge_fn = [&](Block* /* cur */, const Edge* e) {
    if (serialize) {
      auto get_edge_char = [e]() {
        switch (e->type()) {
        case EDGE_BRANCH:
          return 'b';
        case EDGE_GOTO:
          return 'g';
        case EDGE_THROW:
          return 't';
        case EDGE_GHOST:
        case EDGE_TYPE_SIZE:
          not_reached();
        }
        not_reached(); // For GCC.
      };
      oss << " " << get_edge_char();
    }
  };
  auto block_end_fn = [&](Block* /* cur */) {
    if (serialize) {
      oss << ")";
    }
  };
  impl::visit_in_order(cfg, block_start_fn, edge_fn, block_end_fn);

  return {cfg->blocks().size(), oss.str()};
}

} // namespace source_blocks
