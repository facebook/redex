/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string.hpp>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "IRCode.h"
#include "MethodUtil.h"
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

TEST_F(PostVerify, EnsureNewInstanceOrder) {
  auto method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "InstrumentBasicBlockTarget;.testFuncNewInstanceOrder:()V");
  ASSERT_NE(method_ref, nullptr);
  ASSERT_TRUE(method_ref->is_def());
  auto method = method_ref->as_def();
  method->balloon();

  const IRInstruction* last = nullptr;
  for (auto& mie : *method->get_code()) {
    if (mie.type != MFLOW_OPCODE) {
      continue;
    }

    if (mie.insn->opcode() == OPCODE_INVOKE_DIRECT) {
      ASSERT_TRUE(method::is_init(mie.insn->get_method()));
      ASSERT_EQ(mie.insn->get_method()->get_class()->str(),
                "Ljava/lang/String;");
      ASSERT_NE(last, nullptr);

      // The new-instance should not be moved over the const.
      EXPECT_EQ(last->opcode(), OPCODE_CONST);

      break;
    }

    last = mie.insn;
  }
}
