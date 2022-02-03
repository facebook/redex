/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GraphColoring.h"

class DexMethod;

namespace regalloc {
namespace graph_coloring {

// Note: this always destroys any CFG the method might have created.
Allocator::Stats allocate(const Allocator::Config&, DexMethod*);
Allocator::Stats allocate(const Allocator::Config&,
                          IRCode*,
                          bool is_static,
                          const std::function<std::string()>& method_describer);

} // namespace graph_coloring
} // namespace regalloc
