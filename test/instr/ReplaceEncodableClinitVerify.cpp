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
  ASSERT_NE(nullptr, clinit);
}

/*
 * Ensure that we are actually replacing encodable clinits by checking
 * that they exist in the pre-redexed binary.
 */

TEST_F(PreVerify, ReplaceEncodableClinit) {
  assert_class_clinit_exist(classes, "Lredex/Encodable;");
  assert_class_clinit_exist(classes, "Lredex/UnEncodable;");
  assert_class_clinit_exist(classes, "Lredex/HasWides;");
  assert_class_clinit_exist(classes, "Lredex/HasCharSequence;");
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
      {"S_BOOL", "Z", static_cast<uint64_t>(1)},
      {"S_BYTE", "B", static_cast<uint64_t>('b')},
      {"S_CHAR", "C", static_cast<uint64_t>('c')},
      {"S_INT", "I", static_cast<uint64_t>(12345)},
      {"S_SHORT", "S", static_cast<uint64_t>(128)},
      {"S_STRING", "Ljava/lang/String;", static_cast<std::string>("string")}};
  for (const auto& tc : test_cases) {
    auto name = DexString::get_string(tc.name);
    auto type = DexType::get_type(tc.type);
    auto f = resolve_field(enc_type, name, type);
    ASSERT_NE(nullptr, f) << "Failed resolving field " << tc.name;
    auto ev = f->get_static_value();
    ASSERT_NE(nullptr, ev) << "Failed getting value for field " << tc.name;
    if (tc.value.type() == typeid(uint64_t)) {
      ASSERT_EQ(boost::any_cast<uint64_t>(tc.value), ev->value())
          << "Unexpected value for field " << tc.name;
    } else if (tc.value.type() == typeid(std::string)) {
      ASSERT_EQ(boost::any_cast<std::string>(tc.value),
                show(static_cast<DexEncodedValueString*>(ev)->string()))
          << "Unexpected value for field " << tc.name;
    } else {
      ASSERT_FALSE(true);
    }
  }

  assert_class_clinit_exist(classes, "Lredex/UnEncodable;");
  assert_class_clinit_exist(classes, "Lredex/HasWides;");
  assert_class_clinit_exist(classes, "Lredex/HasCharSequence;");
}
