/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "RedexResources.h"
#include "verify/VerifyUtil.h"

// Asserts based on ID ranges that were moved, not moved, type ids that exist
// post transform, and expected string values for certain resources. Actual
// string lookups will need to be handled by caller, which should assert that
// the given ID is truly a string like value.
void postverify_impl(const DexClasses& classes,
                     const std::function<std::vector<std::string>(uint32_t)>&
                         string_value_getter,
                     ResourceTableFile* res_table);
