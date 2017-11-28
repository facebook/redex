/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <boost/any.hpp>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Resolver.h"
#include "Show.h"
#include "VerifyUtil.h"

struct StaticValueTestCase {
  const char* name;
  const char* type;
  const boost::any value;
};

void assert_class_clinit_exist(DexClasses& classes, const char* name) {
  auto cls = find_class_named(classes, name);
  ASSERT_NE(nullptr, cls);
  auto clinit = cls->get_clinit();
  EXPECT_NE(nullptr, clinit);
}

/*
 * Ensure that we are actually replacing encodable clinits by checking
 * that they exist in the pre-redexed binary.
 */

TEST_F(PreVerify, ReplaceEncodableClinit) {
  assert_class_clinit_exist(classes, "Lredex/Encodable;");
  assert_class_clinit_exist(classes, "Lredex/UnEncodable;");
  assert_class_clinit_exist(classes, "Lredex/HasCharSequence;");
}

/*
 * Ensure that we've removed the appropriate clinit and that the corresponding
 * static values are still correct.
 *
 * TODO: Note that Encodable class in ReplaceEncodableClinitTest.java don't have
 * finals. It has just static fields. If you place finals, javac will do the
 * constant propagation and won't generate clinit. That's why our testing class
 * doesn't have final properties. Meanwhile, FinalInline's removing clinit logic
 * has been broken. It mistakely removed non-final fields. It should have
 * removed const-sput pairs with *final* and *static* fields. D5252471 fixes
 * this bug. Unfortunately this diff makes it harder to test with Java source.
 * Hence, we remove the testing for now. We need to write a new unit test
 * instead of instrumentation test.
 */
TEST_F(PostVerify, ReplaceEncodableClinit) {
  // The fixed FinalInline should not remove clinits with non-final fields.
  assert_class_clinit_exist(classes, "Lredex/Encodable;");

  assert_class_clinit_exist(classes, "Lredex/UnEncodable;");
  assert_class_clinit_exist(classes, "Lredex/HasCharSequence;");
}
