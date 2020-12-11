/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string.hpp>
#include <gtest/gtest.h>

#include "Show.h"
#include "VerifyUtil.h"

namespace {
static const char* kTargetClassName =
    "Lcom/facebook/redextest/InstrumentBasicBlockTarget;";

static const char* kNamePrefix =
    "Lcom/facebook/redextest/InstrumentBasicBlockTarget;.testFunc";

auto matcher = [](const DexMethod* method) {
  return boost::starts_with(show(method), kNamePrefix);
};
} // namespace

TEST_F(PreVerify, InstrumentBBVerify) {
  auto cls = find_class_named(classes, kTargetClassName);
  dump_cfgs(true, cls, matcher);
}

TEST_F(PostVerify, InstrumentBBVerify) {
  auto cls = find_class_named(classes, kTargetClassName);
  dump_cfgs(false, cls, matcher);
}
