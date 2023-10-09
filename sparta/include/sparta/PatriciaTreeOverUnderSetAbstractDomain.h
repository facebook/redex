/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/OverUnderSetAbstractDomain.h>
#include <sparta/PatriciaTreeSet.h>

namespace sparta {

/**
 * An implementation of powerset abstract domains that computes both an over-
 * and under-approximation using Patricia trees.
 *
 * This domain can only handle elements that are either unsigned integers or
 * pointers to objects.
 */
template <typename Element>
using PatriciaTreeOverUnderSetAbstractDomain =
    OverUnderSetAbstractDomain<PatriciaTreeSet<Element>>;

} // namespace sparta
