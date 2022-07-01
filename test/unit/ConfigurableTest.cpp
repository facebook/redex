/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Configurable.h"
#include "DexClass.h"
#include "RedexTest.h"

struct Base : public Configurable {
  std::string get_config_name() override { return ""; }
  int m_int_param;
  float m_float_param;
  bool m_bool_param;
  unsigned int m_uint_param;
  int64_t m_int64_param;
  uint64_t m_uint64_param;
  std::string m_string_param;
  Json::Value m_json_param;
  DexType* m_type_param;
  DexType* m_unresolvable_type_param;
  std::vector<std::string> m_vector_of_string_param;
  std::vector<uint32_t> m_vector_of_uint_param;
  boost::optional<uint32_t> m_optional_uint32;
  boost::optional<std::string> m_optional_string;
};

struct BadBindFlags : public Base {
  explicit BadBindFlags(bindflags_t bindflags = 0) { m_bindflags = bindflags; }
  void bind_config() override {
    bind("int_param", 0, m_int_param, "", m_bindflags);
  }
  bindflags_t m_bindflags;
};

class ConfigurableTest : public RedexTest {};

TEST_F(ConfigurableTest, BadBindFlags) {
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

struct OptionalBindings : public Base {
  explicit OptionalBindings(bindflags_t optional_string_bindflags) {
    m_optional_string_bindflags = optional_string_bindflags;
  }
  void bind_config() override {
    bind("optional_uint32_param", {}, m_optional_uint32);
    bind("optional_string_param", {}, m_optional_string, "",
         m_optional_string_bindflags);
  }
  bindflags_t m_optional_string_bindflags;
};

TEST_F(ConfigurableTest, OptionalBindings) {
  {
    Json::Value json;
    OptionalBindings c(0);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(false, (bool)c.m_optional_uint32);
    EXPECT_EQ(false, (bool)c.m_optional_string);
  }
  {
    Json::Value json;
    json["optional_string_param"] = "";
    OptionalBindings c(0);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(false, (bool)c.m_optional_uint32);
    EXPECT_EQ(true, (bool)c.m_optional_string);
    EXPECT_EQ("", *(c.m_optional_string));
  }
  {
    Json::Value json;
    json["optional_string_param"] = "";
    OptionalBindings c(Configurable::bindflags::optionals::skip_empty_string);
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(false, (bool)c.m_optional_uint32);
    EXPECT_EQ(false, (bool)c.m_optional_string);
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
    bind("vector_of_uint_param", {}, m_vector_of_uint_param);
    bind("type_param", {}, m_type_param);
    bind("unresolvable_type_param", {}, m_unresolvable_type_param,
         Configurable::default_doc(),
         Configurable::bindflags::types::warn_if_unresolvable);
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

Json::Value getUintArray() {
  Json::Value array;
  array.append(15);
  array.append(325432);
  array.append(4234324);
  return array;
}

std::vector<uint32_t> getUintsVector() {
  return std::vector<uint32_t>{15, 325432, 4234324};
}

Json::Value getFooBarObject() {
  Json::Value obj;
  obj["foo"] = "bar";
  obj["baz"] = getFooBarBazArray();
  return obj;
}

TEST_F(ConfigurableTest, PrimitiveBindings) {
  DexType::make_type("Ltype1;");

  Json::Value json;
  json["int_param"] = 10;
  json["float_param"] = 11.0f;
  json["bool_param"] = true;
  json["uint_param"] = 0xffffffff;
  json["int64_param"] = Json::Int64(-5000000000);
  json["uint64_param"] = Json::UInt64(5000000000);
  json["string_param"] = "a string";
  json["json_param"] = getFooBarObject();
  json["vector_of_string_param"] = getFooBarBazArray();
  json["vector_of_uint_param"] = getUintArray();
  json["type_param"] = "Ltype1;";
  json["unresolvable_type_param"] = "Ltype2;";

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
  EXPECT_EQ(getUintsVector(), c.m_vector_of_uint_param);
  EXPECT_EQ(DexType::get_type("Ltype1;"), c.m_type_param);
  EXPECT_EQ(nullptr, c.m_unresolvable_type_param);
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
    bind("vector_of_uint_param", getUintsVector(), m_vector_of_uint_param);
  }
};

TEST_F(ConfigurableTest, DefaultBindings) {
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
  EXPECT_EQ(getUintsVector(), c.m_vector_of_uint_param);
}

struct CompositeBindings : public Configurable {
  std::string get_config_name() override { return ""; }
  void bind_config() override {
    bind("contained", DefaultBindings{}, m_contained);
  }
  DefaultBindings m_contained;
};

TEST_F(ConfigurableTest, CompositeBindings) {
  Json::Value json;
  json["contained"]["uint64_param"] = Json::UInt64(7000000000);
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
  EXPECT_EQ(getUintsVector(), c.m_contained.m_vector_of_uint_param);
}

struct TypesBindFlags : public Base {
  explicit TypesBindFlags(bindflags_t bindflags = 0) {
    m_bindflags = bindflags;
  }
  void bind_config() override {
    bind("types_param", {}, m_types_param, "", m_bindflags);
  }
  bindflags_t m_bindflags;
  std::unordered_set<DexType*> m_types_param;
};

TEST_F(ConfigurableTest, TypesBindFlags) {
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
    // Check reflection
    TypesBindFlags c(Configurable::bindflags::types::error_if_unresolvable);
    Configurable::bindflags_t expected_bindflags =
        Configurable::bindflags::types::error_if_unresolvable;
    EXPECT_EQ(expected_bindflags, c.reflect().params["types_param"].bindflags);
  }
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
  explicit MethodsBindFlags(bindflags_t bindflags = 0) {
    m_bindflags = bindflags;
  }
  void bind_config() override {
    bind("methods_param", {}, m_methods_param, "", m_bindflags);
  }
  bindflags_t m_bindflags;
  std::unordered_set<DexMethod*> m_methods_param;
};

TEST_F(ConfigurableTest, MethodsBindFlags) {
  auto m1desc = "Ltype1;.foo:()V";
  auto m2desc = "Ltype2;.foo:()V";
  auto m3desc = "Ltype3;.foo:()V";

  DexMethod::make_method(m1desc);
  DexMethod::make_method(m3desc);

  Json::Value json;

  Json::Value array;
  array.append(m1desc);
  array.append(m2desc);
  array.append(m3desc);

  json["methods_param"] = array;

  DexMethodRef* m1 = DexMethod::get_method(m1desc);
  DexMethod* m3 =
      DexMethod::get_method(m3desc)->make_concrete((DexAccessFlags)0, false);
  std::unordered_set<DexMethod*> resolved_methods = {m3};

  EXPECT_EQ(false, DexMethod::get_method(m1desc)->is_def());
  EXPECT_EQ(true, DexMethod::get_method(m3desc)->is_def());

  {
    // Check reflection
    MethodsBindFlags c(Configurable::bindflags::methods::warn_if_not_def |
                       Configurable::bindflags::methods::warn_if_unresolvable);
    EXPECT_EQ(Configurable::bindflags::methods::warn_if_not_def |
                  Configurable::bindflags::methods::warn_if_unresolvable,
              c.reflect().params["methods_param"].bindflags);
  }
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
    MethodsBindFlags c(Configurable::bindflags::methods::warn_if_not_def |
                       Configurable::bindflags::methods::warn_if_unresolvable);
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
  explicit AfterConfiguration(int iterations)
      : m_after_config_called(false), m_iterations(iterations) {}
  void bind_config() override {
    for (int i = 0; i < m_iterations; ++i) {
      after_configuration([this] { m_after_config_called = true; });
    }
  }
  bool m_after_config_called;
  int m_iterations;
};

TEST_F(ConfigurableTest, AfterConfiguration) {
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

struct MapBindings : public Base {
  void bind_config() override {
    bind("map_of_vector_of_strings_param", {}, m_map_of_vector_strings);
    bind("map_of_methods_param", {}, m_map_of_methods);
    bind("map_of_strings_param", {}, m_map_of_strings);
  }
  MapOfVectorOfStrings m_map_of_vector_strings;
  MapOfMethods m_map_of_methods;
  MapOfStrings m_map_of_strings;
};

TEST_F(ConfigurableTest, MapBindings) {
  {
    Json::Value json;
    MapBindings m;
    m.parse_config(JsonWrapper(json));
    EXPECT_EQ(0, m.m_map_of_vector_strings.size());
    EXPECT_EQ(0, m.m_map_of_methods.size());
    EXPECT_EQ(0, m.m_map_of_strings.size());
  }
  {
    Json::Value map;
    Json::Value arr;
    arr.append("foo");
    arr.append("bar");
    arr.append("baz");
    map["key"] = arr;

    Json::Value json;
    json["map_of_vector_of_strings_param"] = map;
    MapBindings m;
    m.parse_config(JsonWrapper(json));
    EXPECT_EQ(1, m.m_map_of_vector_strings.size());
  }
  {
    auto m1desc = "Ltype1;.foo:()V";
    auto m3desc = "Ltype3;.foo:()V";

    DexMethod::make_method(m1desc);
    DexMethod::make_method(m3desc);

    DexMethod::get_method(m1desc)->make_concrete((DexAccessFlags)0, false);
    DexMethod::get_method(m3desc)->make_concrete((DexAccessFlags)0, false);

    Json::Value map;
    map["Ltype1;.foo:()V"] = "Ltype3;.foo:()V";

    Json::Value json;
    json["map_of_methods_param"] = map;
    MapBindings m;
    m.parse_config(JsonWrapper(json));
    EXPECT_EQ(1, m.m_map_of_methods.size());
  }
  {
    Json::Value map;
    map["key"] = "value";

    Json::Value json;
    json["map_of_strings_param"] = map;
    MapBindings m;
    m.parse_config(JsonWrapper(json));
    EXPECT_EQ(1, m.m_map_of_strings.size());
  }
}

struct RequiredBinds : public Base {
  RequiredBinds() {}
  void bind_config() override {
    bind_required("int_param", m_int_param);
    bind_required("type_param", m_type_param, "",
                  Configurable::bindflags::types::error_if_unresolvable);
    bind("string_param", "", m_string_param);
  }
};

TEST_F(ConfigurableTest, RequiredBinds) {
  const char* type1 = "Ltype1;";
  const char* type2 = "Ltype2;";
  DexType::make_type(type1);

  {
    // Check reflection
    Json::Value json;
    RequiredBinds c;
    EXPECT_EQ(true, c.reflect().params["int_param"].is_required);
    EXPECT_EQ(true, c.reflect().params["type_param"].is_required);
    EXPECT_EQ(false, c.reflect().params["string_param"].is_required);
  }
  {
    // Throws because missing int_param and type_param
    Json::Value json;
    RequiredBinds c;
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because missing type_param
    Json::Value json;
    json["int_param"] = 1;
    RequiredBinds c;
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because missing int_param
    Json::Value json;
    json["type_param"] = std::string(type1);
    RequiredBinds c;
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    // Throws because type_param doesn't resolve
    Json::Value json;
    json["int_param"] = 1;
    json["type_param"] = std::string(type2);
    RequiredBinds c;
    EXPECT_THROW({ c.parse_config(JsonWrapper(json)); }, RedexException);
  }
  {
    Json::Value json;
    json["int_param"] = 1;
    json["type_param"] = std::string(type1);
    RequiredBinds c;
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(1, c.m_int_param);
    EXPECT_EQ(DexType::get_type(type1), c.m_type_param);
  }
}

struct DeductionBinds : public Base {
  DeductionBinds() {}
  void bind_config() override {
    bind("int_param", 123, m_int64_param);
    bind("type_param", nullptr, m_type_param);
  }
};

TEST_F(ConfigurableTest, BindDeduction) {
  {
    Json::Value json;
    DeductionBinds c{};
    c.parse_config(JsonWrapper(json));
    EXPECT_EQ(123, c.m_int64_param);
    EXPECT_EQ(nullptr, c.m_type_param);
  }
}
