/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexInstruction.h"
#include "Match.h"
#include "VerifyUtil.h"

/**
 * Ensure the structures in DelSuperTest.java are as expected
 * following a redex transformation.
 */
TEST_F(PostVerify, DelSuper) {
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  // Should have C1 and 2 C2 still

  auto c1 = find_class_named(classes,
                             "Lcom/facebook/redex/test/instr/DelSuperTest$C1;");
  ASSERT_NE(nullptr, c1);

  auto c2 = find_class_named(classes,
                             "Lcom/facebook/redex/test/instr/DelSuperTest$C2;");
  ASSERT_NE(nullptr, c2);

  // C2.optimized1 and C2.optimized2 should be gone
  // XXX: optimized2() doesn't get delsuper treatment due to inlining of
  // C1.optimize2(?)
  auto&& m2 = !m::any_vmethods(
    m::named<DexMethod>("optimized1")/* ||
    m::named<DexMethod>("optimized2")*/);
  ASSERT_TRUE(m2.matches(c2));

  // C1 and C2 should both have 4 notOptimized* methods
  auto&& m3 = m::any_vmethods(m::named<DexMethod>("notOptimized1")) &&
              m::any_vmethods(m::named<DexMethod>("notOptimized2")) &&
              m::any_vmethods(m::named<DexMethod>("notOptimized3")) &&
              m::any_vmethods(m::named<DexMethod>("notOptimized4"));
  ASSERT_TRUE(m3.matches(c1));
  ASSERT_TRUE(m3.matches(c2));

  // check that the invoke instructions are fixed up as well
  auto test_class =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/DelSuperTest;");
  auto test_opt_1 = find_vmethod_named(*test_class, "testOptimized1");
  int optimized1_count = 0;
  for (auto& insn : test_opt_1->get_dex_code()->get_instructions()) {
    if (dex_opcode::is_invoke(insn->opcode())) {
      auto mop = static_cast<DexOpcodeMethod*>(insn);
      auto m = mop->get_method();
      if (strcmp(m->get_name()->c_str(), "optimized1") == 0) {
        ASSERT_STREQ(m->get_class()->get_name()->c_str(),
                     "Lcom/facebook/redex/test/instr/DelSuperTest$C1;");
        ++optimized1_count;
      }
    }
  }
  ASSERT_EQ(optimized1_count, 3);
}
