/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
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
  const uint64_t value;
};

/*
 * Ensure that we are actually replacing encodable clinits by checking
 * that they exist in the pre-redexed binary.
 */

TEST_F(PreVerify, ReplaceEncodableClinit) {
  auto enc_cls = find_class_named(classes, "Lredex/Encodable;");
  ASSERT_NE(nullptr, enc_cls);
  auto enc_clinit = enc_cls->get_clinit();
  ASSERT_NE(nullptr, enc_clinit);

  auto unenc_cls = find_class_named(classes, "Lredex/UnEncodable;");
  ASSERT_NE(nullptr, unenc_cls);
  auto unenc_clinit = unenc_cls->get_clinit();
  ASSERT_NE(nullptr, unenc_clinit);
}

/*
 * Ensure that we've removed the appropriate clinit and that the corresponding
 * static values are still correct.
 */
TEST_F(PostVerify, ReplaceEncodableClinit) {
  auto enc_cls = find_class_named(classes, "Lredex/Encodable;");
  ASSERT_NE(nullptr, enc_cls);
  auto enc_clinit = enc_cls->get_clinit();
  ASSERT_EQ(nullptr, enc_clinit);
  auto enc_type = enc_cls->get_type();
  ASSERT_NE(nullptr, enc_type);

  StaticValueTestCase test_cases[] = {
    {"S_BOOL", "Z", 1},
    {"S_BYTE", "B", 'b'},
    {"S_CHAR", "C", 'c'},
    {"S_INT", "I", 12345},
    {"S_SHORT", "S", 128}
  };
  for (auto tc : test_cases) {
    auto name = DexString::get_string(tc.name);
    auto type = DexType::get_type(tc.type);
    auto f = resolve_field(enc_type, name, type);
    ASSERT_NE(nullptr, f) << "Failed resolving field " << tc.name;
    auto v = f->get_static_value();
    ASSERT_NE(nullptr, v) << "Failed getting value for field " << tc.name;
    ASSERT_EQ(tc.value, v->value()) << "Unexpected value for field " << tc.name;
  }

  auto name = DexString::get_string("S_STRING");
  auto type = DexType::get_type("Ljava/lang/String;");
  auto f = resolve_field(enc_type, name, type);
  ASSERT_NE(nullptr, f) << "Failed resolving field S_STRING";
  auto ev = f->get_static_value();
  ASSERT_NE(nullptr, ev) << "Failed getting value for field S_STRING";
  auto evs = static_cast<DexEncodedValueString*>(ev);
  std::string expected("Testing 123");
  std::string actual(evs->string()->c_str());
  ASSERT_EQ(expected, actual) << "Incorrect value for field S_STRING";

  auto unenc_cls = find_class_named(classes, "Lredex/UnEncodable;");
  ASSERT_NE(nullptr, unenc_cls);
  auto unenc_clinit = unenc_cls->get_clinit();
  ASSERT_NE(nullptr, unenc_clinit);
}
