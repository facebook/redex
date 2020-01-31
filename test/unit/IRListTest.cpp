/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "IRCode.h"
#include "IRList.h"
#include "RedexTest.h"

class IRListTest : public RedexTest {};

TEST_F(IRListTest, method_item_entry_equality) {
  std::string s_insns = R"(
    (
      (load-param v0)
      (.dbg DBG_SET_PROLOGUE_END)

      (.try_start foo)
      (const v0 0)
      (if-gtz v0 :tru)
      (throw v0)
      (.try_end foo)

      (.catch (foo))
      (const v1 3)
      (return v1)

      (:tru)
      (const v2 2)
      (return v2)

      (return v0)
    )
  )";
  auto code = assembler::ircode_from_string(s_insns);
  auto code_clone = assembler::ircode_from_string(s_insns);

  IRList::iterator code_it = code->begin();
  IRList::iterator clone_it = code_clone->begin();

  while (code_it != code->end() && clone_it != code_clone->end()) {
    EXPECT_TRUE(*code_it == *clone_it);

    code_it++;
    clone_it++;
  }

  always_assert(code_it == code->end() && clone_it == code_clone->end());
}
