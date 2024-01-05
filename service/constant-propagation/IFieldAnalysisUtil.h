/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantPropagationWholeProgramState.h"

namespace constant_propagation {

EligibleIfields gather_safely_inferable_ifield_candidates(
    const Scope& scope,
    const std::unordered_set<std::string>& allowlist_method_names);

} // namespace constant_propagation
