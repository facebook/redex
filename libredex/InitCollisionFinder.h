/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"

namespace init_collision_finder {

using GetNewSpec = std::function<boost::optional<DexMethodSpec>(
    const DexMethod*, std::vector<DexType*>*)>;

std::vector<DexType*> find(const Scope& scope, const GetNewSpec& get_new_spec);

} // namespace init_collision_finder
