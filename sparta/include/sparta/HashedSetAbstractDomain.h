/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/HashSet.h>
#include <sparta/SetAbstractDomain.h>

namespace sparta {

/*
 * An implementation of powerset abstract domains using hash tables.
 */
template <typename Element,
          typename Hash = std::hash<Element>,
          typename Equal = std::equal_to<Element>>
using HashedSetAbstractDomain =
    SetAbstractDomain<HashSet<Element, Hash, Equal>>;

} // namespace sparta
