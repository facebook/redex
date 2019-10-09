/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gtest/gtest.h>

#include "RedexContext.h"

struct RedexTest : public testing::Test {
  RedexTest() { g_redex = new RedexContext(); }

  ~RedexTest() { delete g_redex; }
};
