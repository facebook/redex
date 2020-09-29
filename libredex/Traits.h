/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace Traits {
namespace Pass {

// the pass may be run zero or one times (bool)
constexpr auto unique = "unique";

// the pass must be run at least N times (int)
constexpr auto atleast = "atleast";

}; // namespace Pass
}; // namespace Traits
