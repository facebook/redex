/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "Creators.h"
#include "DexClass.h"
#include "ProguardConfiguration.h"
#include "ProguardMatcher.h"
#include "RedexTest.h"

using namespace keep_rules;

using NameSpec = ClassSpecification::ClassNameSpec;

class ProguardMatcherTest : public RedexTest {
 public:
  static ClassSpecification create_class_spec(
      DexAccessFlags setAccessFlags = DexAccessFlags(0),
      DexAccessFlags unsetAccessFlags = DexAccessFlags(0),
      std::string annotationType = "",
      std::vector<NameSpec> classNames = {},
      std::string extendsAnnotationType = "",
      std::string extendsClassName = "",
      std::vector<MemberSpecification> fieldSpecifications = {},
      std::vector<MemberSpecification> methodSpecifications = {}) {
    ClassSpecification class_spec;
    class_spec.setAccessFlags = setAccessFlags;
    class_spec.unsetAccessFlags = unsetAccessFlags;
    class_spec.annotationType = std::move(annotationType);
    class_spec.classNames = std::move(classNames);
    class_spec.extendsAnnotationType = std::move(extendsAnnotationType);
    class_spec.extendsClassName = std::move(extendsClassName);
    class_spec.fieldSpecifications = std::move(fieldSpecifications);
    class_spec.methodSpecifications = std::move(methodSpecifications);
    return class_spec;
  }

  static ClassSpecification create_class_spec(
      std::vector<NameSpec> classNames) {
    return create_class_spec(DexAccessFlags(0),
                             DexAccessFlags(0),
                             "",
                             std::move(classNames),
                             "",
                             "",
                             {},
                             {});
  }

  static std::unique_ptr<KeepSpec> create_spec(ClassSpecification class_spec) {
    auto res = std::make_unique<KeepSpec>();
    res->class_spec = std::move(class_spec);
    return res;
  }

  static DexClass* create_class(
      const std::string& name,
      std::optional<std::string> super_klass = std::nullopt) {
    auto existing_type = DexType::get_type(name);
    if (existing_type != nullptr) {
      auto existing_class = type_class(existing_type);
      if (existing_class != nullptr) {
        return existing_class;
      }
    }

    ClassCreator cc{DexType::make_type(name)};
    cc.set_super(super_klass ? DexType::make_type(*super_klass)
                             : type::java_lang_Object());
    return cc.create();
  }

  static bool matches(const KeepSpec& ks, const std::string& class_name) {
    auto* klass = create_class(class_name);
    return keep_rules::testing::matches(ks, klass);
  }
};

// Make sure we can parse an empty string
TEST_F(ProguardMatcherTest, exact_class) {
  auto ks = create_spec(create_class_spec({NameSpec("Foo", false)}));

  EXPECT_TRUE(matches(*ks, "LFoo;"));
  EXPECT_FALSE(matches(*ks, "LBar;"));
}

TEST_F(ProguardMatcherTest, star_class) {
  {
    auto ks = create_spec(create_class_spec({NameSpec("*", false)}));

    EXPECT_TRUE(matches(*ks, "LFoo;"));
    EXPECT_TRUE(matches(*ks, "LBar;"));
  }

  {
    auto ks = create_spec(create_class_spec({NameSpec("Foo*", false)}));

    EXPECT_TRUE(matches(*ks, "LFoo;"));
    EXPECT_TRUE(matches(*ks, "LFoo1;"));
    EXPECT_FALSE(matches(*ks, "LBar1;"));
  }
}

TEST_F(ProguardMatcherTest, list_class) {
  auto ks = create_spec(
      create_class_spec({NameSpec("Bar", false), NameSpec("Foo", false)}));

  EXPECT_TRUE(matches(*ks, "LFoo;"));
  EXPECT_TRUE(matches(*ks, "LBar;"));
  EXPECT_FALSE(matches(*ks, "LBaz;"));
}

TEST_F(ProguardMatcherTest, list_star_class) {
  auto ks = create_spec(
      create_class_spec({NameSpec("Bar", false), NameSpec("Foo*", false)}));

  EXPECT_TRUE(matches(*ks, "LFoo;"));
  EXPECT_TRUE(matches(*ks, "LFoo2;"));
  EXPECT_TRUE(matches(*ks, "LBar;"));
  EXPECT_FALSE(matches(*ks, "LBaz;"));
}

TEST_F(ProguardMatcherTest, negate_class) {
  auto ks = create_spec(
      create_class_spec({NameSpec("Bar", true), NameSpec("Foo", false)}));

  EXPECT_TRUE(matches(*ks, "LFoo;"));
  EXPECT_FALSE(matches(*ks, "LBar;"));
}

TEST_F(ProguardMatcherTest, negate_star_class) {
  {
    auto ks = create_spec(
        create_class_spec({NameSpec("FooBar", true), NameSpec("Foo*", false)}));

    EXPECT_TRUE(matches(*ks, "LFoo;"));
    EXPECT_TRUE(matches(*ks, "LFoo1;"));
    EXPECT_FALSE(matches(*ks, "LFooBar;"));
    EXPECT_TRUE(matches(*ks, "LFooBar1;"));
  }

  {
    auto ks = create_spec(
        create_class_spec({NameSpec("FooB*", true), NameSpec("Foo*", false)}));

    EXPECT_TRUE(matches(*ks, "LFoo;"));
    EXPECT_TRUE(matches(*ks, "LFoo1;"));
    EXPECT_FALSE(matches(*ks, "LFooBar;"));
    EXPECT_FALSE(matches(*ks, "LFooBaz;"));
  }

  {
    auto ks = create_spec(
        create_class_spec({NameSpec("Foo*", false), NameSpec("FooBar", true)}));

    EXPECT_TRUE(matches(*ks, "LFoo;"));
    EXPECT_TRUE(matches(*ks, "LFoo1;"));
    EXPECT_TRUE(matches(*ks, "LFooBar;"));
    EXPECT_TRUE(matches(*ks, "LFooBaz;"));
  }
}

TEST_F(ProguardMatcherTest, negate_class_longer_list) {
  auto ks = create_spec(create_class_spec({
      NameSpec("F*", true),
      NameSpec("H*", false),
      NameSpec("HA*", true), // Should not matter, above applies first.
      NameSpec("Ioo*", true),
      NameSpec("I*", false),
      NameSpec("Joo*", true),
      NameSpec("J*", false),
  }));

  EXPECT_FALSE(matches(*ks, "LFoo;"));

  EXPECT_TRUE(matches(*ks, "LHoo;"));
  EXPECT_TRUE(matches(*ks, "LHA;"));

  EXPECT_TRUE(matches(*ks, "LI;"));
  EXPECT_TRUE(matches(*ks, "LIo;"));
  EXPECT_TRUE(matches(*ks, "LIo1;"));
  EXPECT_FALSE(matches(*ks, "LIoo;"));
  EXPECT_FALSE(matches(*ks, "LIoo1;"));

  EXPECT_TRUE(matches(*ks, "LJ;"));
  EXPECT_TRUE(matches(*ks, "LJo;"));
  EXPECT_TRUE(matches(*ks, "LJo1;"));
  EXPECT_FALSE(matches(*ks, "LJoo;"));
  EXPECT_FALSE(matches(*ks, "LJoo1;"));
}
