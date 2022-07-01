/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "VerifyUtil.h"

static bool HasCatchBlock(IRCode* code) {
  code->build_cfg(/* editable */ false);
  bool has_catch = false;
  for (auto it : code->cfg().blocks()) {
    if (it->is_catch()) {
      has_catch = true;
    }
  }
  code->clear_cfg();
  return has_catch;
}

TEST_F(PreVerify, CheckRecursionTest) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redextest/CheckRecursionTest;");
  ASSERT_NE(nullptr, cls);

  for (auto& meth : cls->get_vmethods()) {
    if (meth->get_name()->str() == "f1" || meth->get_name()->str() == "f2" ||
        meth->get_name()->str() == "foo") {
      IRCode* code = new IRCode(meth);
      ASSERT_NE(code, nullptr);
      ASSERT_FALSE(HasCatchBlock(code));
    } else if (meth->get_name()->str() == "f3") {
      IRCode* code = new IRCode(meth);
      ASSERT_NE(code, nullptr);
      ASSERT_TRUE(HasCatchBlock(code));
    }
  }
}

TEST_F(PostVerify, CheckRecursionTest) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redextest/CheckRecursionTest;");
  ASSERT_NE(nullptr, cls);

  for (auto& meth : cls->get_vmethods()) {
    if (meth->get_name()->str() == "f1" || meth->get_name()->str() == "f3" ||
        meth->get_name()->str() == "foo") {
      IRCode* code = new IRCode(meth);
      ASSERT_NE(code, nullptr);
      ASSERT_TRUE(HasCatchBlock(code));
    } else if (meth->get_name()->str() == "f2") {
      IRCode* code = new IRCode(meth);
      ASSERT_NE(code, nullptr);
      ASSERT_FALSE(HasCatchBlock(code));
    }
  }
}
