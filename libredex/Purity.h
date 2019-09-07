/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

/*
 * Pure methods...
 * - do not read or write mutable state...
 * - ... in a way that could be observed (by reading state or calling other
 *   methods); so we are actually talking about a notion of "observational
 *   purity" here
 * - are deterministic (and do not return newly allocated objects, unless object
 *                      identity should be truly irrelevant, such as in the case
 *                      of boxing certain values)
 * - may throw trivial exceptions such as null-pointer exception that
 *   generally shouldn't be caught, or return normally
 *
 * If their outputs are not used, pure method invocations can be removed by DCE.
 * Redundant invocations with same incoming arguments can be eliminated by CSE.
 *
 * Note that this notion of pure methods is different from ProGuard's
 * notion of assumenosideeffects. The latter includes methods that may read
 * mutable state, as well as non-deterministic methods.
 *
 * TODO: Derive this list with static analysis rather than hard-coding
 * it.
 */
std::unordered_set<DexMethodRef*> get_pure_methods();
