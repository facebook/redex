/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GraphColoring.h"

class DexMethod;

namespace regalloc {
namespace graph_coloring {

Allocator::Stats allocate(const Allocator::Config&, DexMethod*);

} // namespace graph_coloring
} // namespace regalloc
