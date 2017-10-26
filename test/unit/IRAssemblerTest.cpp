/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"

TEST(IRAssembler, disassemble) {
  g_redex = new RedexContext();

  auto code = assembler::ircode_from_string(R"(
    (
     (const/4 v0 0)
     :foo-label
     (if-eqz v0 :foo-label)
     (invoke-virtual (v0 v1) "LFoo;.bar:(II)V")
     (sget-object "LFoo;.qux:LBar;")
     (move-result-pseudo-object v0)
     (return-void)
    )
)");
  auto s = assembler::to_string(code.get());
  EXPECT_EQ(s,
            "((const/4 v0 0) "
            ":L0 "
            "(if-eqz v0 :L0) "
            "(invoke-virtual (v0 v1) \"LFoo;.bar:(II)V\") "
            "(sget-object \"LFoo;.qux:LBar;\") "
            "(move-result-pseudo-object v0) "
            "(return-void))");
  EXPECT_EQ(s, assembler::to_string(assembler::ircode_from_string(s).get()));

  delete g_redex;
}
