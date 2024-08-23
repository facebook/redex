/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "Show.h"
#include "verify/VerifyUtil.h"

using namespace testing;

namespace {
std::set<std::string> SUPPORTED_FIELDS{"L1", "L4", "L8"};
std::set<std::string> UNSUPPORTED_FIELDS{"L2", "L3", "L5", "L6", "L7"};
} // namespace

TEST_F(PreVerify, VerifyBaseState) {
  auto wrapped_cls = find_class_named(classes, "Lcom/facebook/redex/MyLong;");
  auto wrapped_type = wrapped_cls->get_type();
  auto cls = find_class_named(classes, "Lcom/facebook/redex/AllValues;");

  std::vector<std::string> all_fields;
  all_fields.insert(all_fields.end(), SUPPORTED_FIELDS.begin(),
                    SUPPORTED_FIELDS.end());
  all_fields.insert(all_fields.end(), UNSUPPORTED_FIELDS.begin(),
                    UNSUPPORTED_FIELDS.end());
  for (const auto& name : all_fields) {
    auto f = find_sfield_named(*cls, name.c_str());
    EXPECT_NE(f, nullptr) << "Did not find field " << name;
    EXPECT_EQ(f->get_type(), wrapped_type);
  }
}

TEST_F(PostVerify, VerifyTransform) {
  auto wrapped_cls = find_class_named(classes, "Lcom/facebook/redex/MyLong;");
  auto wrapped_type = wrapped_cls->get_type();
  auto primitive_long = type::_long();
  auto cls = find_class_named(classes, "Lcom/facebook/redex/AllValues;");
  for (const auto& name : SUPPORTED_FIELDS) {
    auto f = find_sfield_named(*cls, name.c_str());
    EXPECT_NE(f, nullptr) << "Did not find field " << name;
    EXPECT_EQ(f->get_type(), primitive_long)
        << "Field " << SHOW(f) << " should be unboxed!";
  }
  for (const auto& name : UNSUPPORTED_FIELDS) {
    auto f = find_sfield_named(*cls, name.c_str());
    EXPECT_NE(f, nullptr) << "Did not find field " << name;
    EXPECT_EQ(f->get_type(), wrapped_type)
        << "Field " << SHOW(f) << " should be unchanged!";
  }

  auto usage_cls =
      find_class_named(classes, "Lcom/facebook/redex/WrappedPrimitives;");
  auto run = find_method_named(*usage_cls, "run");
  EXPECT_NE(run, nullptr);
  run->balloon();
  run->get_code()->build_cfg();
  auto& cfg = run->get_code()->cfg();
  std::cout << show(cfg) << std::endl;
}
