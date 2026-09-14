/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace regalloc {

// After live-range renumbering, materializes check-cast joins whose result web
// is also live in an exception handler. Returns the number of copies inserted.
size_t split_check_cast_result_live_ranges(cfg::ControlFlowGraph& cfg);

} // namespace regalloc
