/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Configurable.h"
#include "DexClass.h"

struct Base : public Configurable {
  std::string get_config_name() override { return ""; }
  int m_int_param;
  float m_float_param;
  bool m_bool_param;
  uint m_uint_param;
  int64_t m_int64_param;
  uint64_t m_uint64_param;
  std::string m_string_param;
  Json::Value m_json_param;
  std::vector<std::string> m_vector_of_string_param;
};

struct BadBindFlags : public Base {
  BadBindFlags(bindflags_t bindflags = 0) { m_bindflags = bindflags; }
  void bind_config() override {
    bind("int_param", 0, m_int_param, "", m_bindflags);
  }
  bindflags_t m_bindflags;
};

TEST(Configurable, BadBindFlags) {
  Json::Value json;
  json["int_param"] = 10;

  {
    // Throws because methods bindflags are not allowable here
    BadBindFlags bbf(Configurable::bindflags::methods::mask);
    EXPECT_THROW({ bbf.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because types bindflags are not allowable here
    BadBindFlags bbf(Configurable::bindflags::types::mask);
    EXPECT_THROW({ bbf.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because classes bindflags are not allowable here
    BadBindFlags bbf(Configurable::bindflags::classes::mask);
    EXPECT_THROW({ bbf.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because optionals bindflags are not allowable here
    BadBindFlags bbf(Configurable::bindflags::optionals::mask);
    EXPECT_THROW({ bbf.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    BadBindFlags bbf;
    bbf.parse_config(JsonWrapper(json));
    EXPECT_EQ(10, bbf.m_int_param);
  }
}

struct PrimitiveBindings : public Base {
  void bind_config() override {
    bind("int_param", {0}, m_int_param);
    bind("float_param", {0}, m_float_param);
    bind("bool_param", false, m_bool_param);
    bind("uint_param", {0}, m_uint_param);
    bind("int64_param", {0}, m_int64_param);
    bind("uint64_param", {0}, m_uint64_param);
    bind("string_param", "", m_string_param);
    bind("json_param", {}, m_json_param);
    bind("vector_of_string_param", {}, m_vector_of_string_param);
  }
};

Json::Value getFooBarBazArray() {
  Json::Value array;
  array.append("foo");
  array.append("bar");
  array.append("baz");
  return array;
}

std::vector<std::string> getFooBarBazVector() {
  return std::vector<std::string>{"foo", "bar", "baz"};
}

Json::Value getFooBarObject() {
  Json::Value obj;
  obj["foo"] = "bar";
  obj["baz"] = getFooBarBazArray();
  return obj;
}

TEST(Configurable, PrimitiveBindings) {
  Json::Value json;
  json["int_param"] = 10;
  json["float_param"] = 11.0f;
  json["bool_param"] = true;
  json["uint_param"] = 0xffffffff;
  json["int64_param"] = {-5000000000};
  json["uint64_param"] = {5000000000};
  json["string_param"] = "a string";
  json["json_param"] = getFooBarObject();
  json["vector_of_string_param"] = getFooBarBazArray();

  PrimitiveBindings c;
  c.parse_config(JsonWrapper(json));
  EXPECT_EQ(10, c.m_int_param);
  EXPECT_EQ(11.0f, c.m_float_param);
  EXPECT_EQ(true, c.m_bool_param);
  EXPECT_EQ(0xffffffff, c.m_uint_param);
  EXPECT_EQ(-5000000000, c.m_int64_param);
  EXPECT_EQ(5000000000, c.m_uint64_param);
  EXPECT_EQ("a string", c.m_string_param);
  EXPECT_EQ(getFooBarObject(), c.m_json_param);
  EXPECT_EQ(getFooBarBazVector(), c.m_vector_of_string_param);
}

struct DefaultBindings : public Base {
  void bind_config() override {
    bind("int_param", {10}, m_int_param);
    bind("float_param", {11.0f}, m_float_param);
    bind("bool_param", true, m_bool_param);
    bind("uint_param", {0xffffffff}, m_uint_param);
    bind("int64_param", {-5000000000}, m_int64_param);
    bind("uint64_param", {5000000000}, m_uint64_param);
    bind("string_param", "a string", m_string_param);
    bind("json_param", getFooBarObject(), m_json_param);
    bind("vector_of_string_param", getFooBarBazVector(),
         m_vector_of_string_param);
  }
};

TEST(Configurable, DefaultBindings) {
  Json::Value json;

  DefaultBindings c;
  c.parse_config(JsonWrapper(json));
  EXPECT_EQ(10, c.m_int_param);
  EXPECT_EQ(11.0f, c.m_float_param);
  EXPECT_EQ(true, c.m_bool_param);
  EXPECT_EQ(0xffffffff, c.m_uint_param);
  EXPECT_EQ(-5000000000, c.m_int64_param);
  EXPECT_EQ(5000000000, c.m_uint64_param);
  EXPECT_EQ("a string", c.m_string_param);
  EXPECT_EQ(getFooBarObject(), c.m_json_param);
  EXPECT_EQ(getFooBarBazVector(), c.m_vector_of_string_param);
}

struct CompositeBindings : public Configurable {
  std::string get_config_name() override { return ""; }
  void bind_config() override {
    bind("contained", DefaultBindings{}, m_contained);
  }
  DefaultBindings m_contained;
};

TEST(Configurable, CompositeBindings) {
  Json::Value json;
  json["contained"]["uint64_param"] = 7000000000;
  json["contained"]["string_param"] = "a different string";

  CompositeBindings c;
  c.parse_config(JsonWrapper(json));
  EXPECT_EQ(10, c.m_contained.m_int_param);
  EXPECT_EQ(11.0f, c.m_contained.m_float_param);
  EXPECT_EQ(true, c.m_contained.m_bool_param);
  EXPECT_EQ(0xffffffff, c.m_contained.m_uint_param);
  EXPECT_EQ(-5000000000, c.m_contained.m_int64_param);
  EXPECT_EQ(7000000000, c.m_contained.m_uint64_param);
  EXPECT_EQ("a different string", c.m_contained.m_string_param);
  EXPECT_EQ(getFooBarObject(), c.m_contained.m_json_param);
  EXPECT_EQ(getFooBarBazVector(), c.m_contained.m_vector_of_string_param);
}

struct TypesBindFlags : public Base {
  TypesBindFlags(bindflags_t bindflags = 0) { m_bindflags = bindflags; }
  void bind_config() override {
    bind("types_param", {}, m_types_param, "", m_bindflags);
  }
  bindflags_t m_bindflags;
  std::unordered_set<DexType*> m_types_param;
};

TEST(Configurable, TypesBindFlags) {
  g_redex = new RedexContext();
  DexType::make_type("Ltype1;");
  DexType::make_type("Ltype3;");

  Json::Value json;

  Json::Value array;
  array.append("Ltype1;");
  array.append("Ltype2;");
  array.append("Ltype3;");

  json["types_param"] = array;

  std::unordered_set<DexType*> resolved_types = {DexType::get_type("Ltype1;"),
                                                 DexType::get_type("Ltype3;")};

  {
    // Throws because type2 is not resolvable
    TypesBindFlags c(Configurable::bindflags::types::error_if_unresolvable);
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    TypesBindFlags c(Configurable::bindflags::types::warn_if_unresolvable);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(resolved_types, c.m_types_param);
  }
  {
    TypesBindFlags c;
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(resolved_types, c.m_types_param);
  }
}

struct MethodsBindFlags : public Base {
  MethodsBindFlags(bindflags_t bindflags = 0) { m_bindflags = bindflags; }
  void bind_config() override {
    bind("methods_param", {}, m_methods_param, "", m_bindflags);
  }
  bindflags_t m_bindflags;
  std::unordered_set<DexMethod*> m_methods_param;
};

TEST(Configurable, MethodsBindFlags) {
  auto m1desc = "Ltype1;.foo:()V";
  auto m2desc = "Ltype2;.foo:()V";
  auto m3desc = "Ltype3;.foo:()V";

  g_redex = new RedexContext();
  DexMethod::make_method(m1desc);
  DexMethod::make_method(m3desc);

  Json::Value json;

  Json::Value array;
  array.append(m1desc);
  array.append(m2desc);
  array.append(m3desc);

  json["methods_param"] = array;

  DexMethodRef* m1 = DexMethod::get_method(m1desc);
  DexMethodRef* m3 = DexMethod::get_method(m3desc);
  static_cast<DexMethod*>(m3)->make_concrete((DexAccessFlags)0, false);
  std::unordered_set<DexMethod*> resolved_methods = { static_cast<DexMethod*>(m3) };

  EXPECT_EQ(false, DexMethod::get_method(m1desc)->is_def());
  EXPECT_EQ(true, DexMethod::get_method(m3desc)->is_def());

  {
    // Throws because type1;.foo is a ref
    MethodsBindFlags c(Configurable::bindflags::methods::error_if_not_def);
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because type2;.foo is not resolvable
    MethodsBindFlags c(Configurable::bindflags::methods::error_if_unresolvable);
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    MethodsBindFlags c(Configurable::bindflags::methods::warn_if_not_def|Configurable::bindflags::methods::warn_if_unresolvable);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(resolved_methods, c.m_methods_param);
  }
  {
    MethodsBindFlags c;
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(resolved_methods, c.m_methods_param);
  }
}

struct AfterConfiguration : public Base {
  AfterConfiguration(int iterations)
      : m_after_config_called(false), m_iterations(iterations) {}
  void bind_config() override {
    for (int i = 0; i < m_iterations; ++i) {
      after_configuration([this] { m_after_config_called = true; });
    }
  }
  bool m_after_config_called;
  int m_iterations;
};

TEST(Configurable, AfterConfiguration) {
  Json::Value json;

  {
    // 2x after_configuration() + 1x parse_config fails
    AfterConfiguration c(2);
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // 1x after_configuration() + 1x parse_config works
    AfterConfiguration c(1);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(true, c.m_after_config_called);
  }
  {
    // 1x after_configuration() + 2x parse_config works
    AfterConfiguration c(1);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(true, c.m_after_config_called);
    c.m_after_config_called = false;

    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(true, c.m_after_config_called);
    c.m_after_config_called = false;
  }
  {
    // 0x after_configuration() + 1x parse_config value is unset
    AfterConfiguration c(0);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(false, c.m_after_config_called);
  }
}
