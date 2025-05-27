/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ResourceValueMergingPassVerifyImpl.h"
#include "verify/VerifyUtil.h"

TEST_F(PreVerify, ResourceValueMergingPassTest) {
  resource_value_merging_PreVerify();
}

TEST_F(PostVerify, ResourceValueMergingPassTest) {
  resource_value_merging_PostVerify();
}
