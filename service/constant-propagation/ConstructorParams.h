/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <vector>

class DexClass;
using Scope = std::vector<DexClass*>;

namespace constant_propagation {
struct ImmutableAttributeAnalyzerState;
namespace immutable_state {
void analyze_constructors(const Scope& scope,
                          ImmutableAttributeAnalyzerState* state);
} // namespace immutable_state
} // namespace constant_propagation
