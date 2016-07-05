/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>

#include "Match.h"
#include "VerifyUtil.h"

/**
 * Ensure the structures in DelSuperTest.java are as expected
 * following a redex transformation.
 */
TEST_F(PostVerify, DelSuper) {
  std::cout << "Loaded classes: " << classes.size() << std::endl ;

  // Should have C1 and 2 C2 still

  auto c1 = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/DelSuperTest$C1;");
  ASSERT_NE(nullptr, c1);

  auto c2 = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/DelSuperTest$C2;");
  ASSERT_NE(nullptr, c2);

  // C2.optimized1 and C2.optimized2 should be gone
  // XXX: optimized2() doesn't get delsuper treatment due to inlining of C1.optimize2(?)
  auto&& m2 = !m::any_vmethods(
    m::named<DexMethod>("optimized1")/* ||
    m::named<DexMethod>("optimized2")*/);
  ASSERT_TRUE(m2.matches(c2));

  // C1 and C2 should both have 4 notOptimized* methods
  auto&& m3 =
    m::any_vmethods(m::named<DexMethod>("notOptimized1")) &&
    m::any_vmethods(m::named<DexMethod>("notOptimized2")) &&
    m::any_vmethods(m::named<DexMethod>("notOptimized3")) &&
    m::any_vmethods(m::named<DexMethod>("notOptimized4"));
  ASSERT_TRUE(m3.matches(c1));
  ASSERT_TRUE(m3.matches(c2));
}
