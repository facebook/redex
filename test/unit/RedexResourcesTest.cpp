/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ApkResources.h"
#include "Debug.h"
#include "RedexResources.h"
#include "RedexTest.h"
#include "ResourcesTestDefs.h"
#include "androidfw/ResourceTypes.h"

TEST(RedexResources, ReadXmlTagsAndAttributes) {
  UnorderedSet<std::string> attributes_to_find;
  attributes_to_find.emplace("android:onClick");
  attributes_to_find.emplace("onClick");
  attributes_to_find.emplace("android:text");

  resources::StringOrReferenceSet classes;
  std::unordered_multimap<std::string, resources::StringOrReference>
      attribute_values;

  ApkResources resources("");
  resources.collect_layout_classes_and_attributes_for_file(
      get_env("test_layout_path"),
      attributes_to_find,
      &classes,
      &attribute_values);

  EXPECT_EQ(classes.size(), 3);
  EXPECT_EQ(count_strings(classes, "com.example.test.CustomViewGroup"), 1);
  EXPECT_EQ(count_strings(classes, "com.example.test.CustomTextView"), 1);
  EXPECT_EQ(count_strings(classes, "com.example.test.CustomButton"), 1);

  auto method_names =
      string_values_for_key(attribute_values, "android:onClick");
  EXPECT_EQ(method_names.size(), 2);
  EXPECT_EQ(method_names.count("fooClick"), 1);
  EXPECT_EQ(method_names.count("barClick"), 1);

  EXPECT_EQ(count_for_key(attribute_values, "android:text"), 4);
  EXPECT_EQ(count_for_key(attribute_values, "onClick"), 0);

  // Parse another file with slightly different form.
  resources::StringOrReferenceSet more_classes;
  std::unordered_multimap<std::string, resources::StringOrReference>
      more_attribute_values;
  resources.collect_layout_classes_and_attributes_for_file(
      get_env("another_layout_path"),
      {},
      &more_classes,
      &more_attribute_values);
  EXPECT_EQ(more_classes.size(), 5);
  EXPECT_EQ(count_strings(more_classes, "com.facebook.BananaView"), 1);
  EXPECT_EQ(count_strings(more_classes,
                          "androidx.fragment.app.FragmentContainerView"),
            1);
  EXPECT_EQ(count_strings(more_classes, "com.facebook.SomeFragment"), 1);
  EXPECT_EQ(count_strings(more_classes, "com.facebook.AnotherFragment"), 1);
  EXPECT_EQ(count_strings(more_classes, "com.facebook.CoolView"), 1);
}

inline uint32_t to_uint(char c) {
  return static_cast<uint32_t>(static_cast<uint8_t>(c));
}

TEST(RedexResources, Mutf8Conversion) {
  bool be_noisy{false};
  auto verify = [&](const std::string& input,
                    const std::vector<uint8_t>& expected_bytes) {
    auto converted = resources::convert_utf8_to_mutf8(input);
    EXPECT_EQ(converted.size(), expected_bytes.size());
    size_t i = 0;
    for (auto c : converted) {
      EXPECT_EQ(static_cast<uint8_t>(c), expected_bytes[i++]);
      if (be_noisy) {
        std::cout << "GOT CHAR: 0x" << std::hex << to_uint(c) << std::dec
                  << std::endl;
      }
    }
    if (be_noisy) {
      std::cout << "Original:  " << input << std::endl
                << "Converted: " << converted << std::endl;
    }
  };

  // Code points beyond U+FFFF
  verify(u8"Hello, \U0001F30E!",
         {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, 0xed, 0xa0, 0xbc, 0xed,
          0xbc, 0x8e, 0x21});
  verify(
      u8"\U0001F525\U0001F525",
      {0xed, 0xa0, 0xbd, 0xed, 0xb4, 0xa5, 0xed, 0xa0, 0xbd, 0xed, 0xb4, 0xa5});

  // Embedded null
  std::ostringstream embedded_null;
  embedded_null << "yo" << '\0' << "sup";
  verify(embedded_null.str(), {0x79, 0x6f, 0xc0, 0x80, 0x73, 0x75, 0x70});

  // Regular UTF-8 string with one, two, three byte encoded code points that is
  // not changed
  verify(u8"e\u0205\u1E15", {0x65, 0xc8, 0x85, 0xe1, 0xb8, 0x95});
}

TEST(RedexResources, GetResourcesToKeep) {
  UnorderedSet<std::string> single_class_to_keep =
      resources::parse_keep_xml_file(get_env("single_resource_inclusion_path"));

  UnorderedSet<std::string> expected_single_class{"CronetProviderClassName"};

  EXPECT_EQ(expected_single_class, single_class_to_keep);

  UnorderedSet<std::string> expected_multiple_class{
      "CronetProviderClassName", "FooProviderClassName", "BarProviderClassName",
      "AnakinProviderClassName"};
  UnorderedSet<std::string> multiple_class_to_keep_and_spacing =
      resources::parse_keep_xml_file(
          get_env("multiple_resource_inclusion_path"));
  EXPECT_EQ(expected_multiple_class, multiple_class_to_keep_and_spacing);

  UnorderedSet<std::string> expected_empty_class{};
  UnorderedSet<std::string> empty_class_to_keep =
      resources::parse_keep_xml_file(get_env("empty_resource_inclusion_path"));
  EXPECT_EQ(expected_empty_class, empty_class_to_keep);
}

TEST(RedexResources, StyleResourceSpan) {
  resources::StyleResource::Value::Span span1{"bold", 0, 5};
  resources::StyleResource::Value::Span span2{"bold", 0, 5};
  EXPECT_TRUE(span1 == span2);

  resources::StyleResource::Value::Span span3{"italic", 0, 5};
  EXPECT_FALSE(span1 == span3);

  resources::StyleResource::Value::Span span4{"bold", 1, 5};
  EXPECT_FALSE(span1 == span4);

  resources::StyleResource::Value::Span span5{"bold", 0, 6};
  EXPECT_FALSE(span1 == span5);
}

TEST(RedexResources, StyleResourceValueBytes) {
  const uint8_t data_type = 1;
  const uint32_t bytes_value = 0x12345678;

  resources::StyleResource::Value value1(data_type, bytes_value);
  resources::StyleResource::Value value2(data_type, bytes_value);

  EXPECT_TRUE(value1 == value2);

  resources::StyleResource::Value value3(data_type + 1, bytes_value);
  EXPECT_FALSE(value1 == value3);

  resources::StyleResource::Value value4(data_type, bytes_value + 1);
  EXPECT_FALSE(value1 == value4);
}

TEST(RedexResources, StyleResourceValueString) {
  const uint8_t data_type = 2;
  const std::string str_value = "test string";

  resources::StyleResource::Value value1(data_type, str_value);
  resources::StyleResource::Value value2(data_type, str_value);

  EXPECT_TRUE(value1 == value2);

  resources::StyleResource::Value value3(data_type + 1, str_value);
  EXPECT_FALSE(value1 == value3);

  resources::StyleResource::Value value4(data_type, str_value + " modified");
  EXPECT_FALSE(value1 == value4);

  const std::string empty_str;
  resources::StyleResource::Value value5(data_type, empty_str);
  resources::StyleResource::Value value6(data_type, empty_str);
  EXPECT_TRUE(value5 == value6);
  EXPECT_FALSE(value1 == value5);
}

TEST(RedexResources, StyleResourceValueStyled) {
  const uint8_t data_type = 3;
  std::vector<resources::StyleResource::Value::Span> spans1 = {
      {"bold", 0, 5}, {"italic", 6, 10}};

  resources::StyleResource::Value value1(data_type, spans1);

  std::vector<resources::StyleResource::Value::Span> spans2 = {
      {"bold", 0, 5}, {"italic", 6, 10}};
  resources::StyleResource::Value value2(data_type, spans2);

  EXPECT_TRUE(value1 == value2);

  resources::StyleResource::Value value3(data_type + 1, spans1);
  EXPECT_FALSE(value1 == value3);

  std::vector<resources::StyleResource::Value::Span> spans3 = {{"bold", 0, 5}};
  resources::StyleResource::Value value4(data_type, spans3);
  EXPECT_FALSE(value1 == value4);

  std::vector<resources::StyleResource::Value::Span> spans4 = {
      {"bold", 0, 5}, {"underline", 6, 10}};
  resources::StyleResource::Value value5(data_type, spans4);
  EXPECT_FALSE(value1 == value5);

  std::vector<resources::StyleResource::Value::Span> empty_spans;
  resources::StyleResource::Value value6(data_type, empty_spans);
  resources::StyleResource::Value value7(data_type, empty_spans);
  EXPECT_TRUE(value6 == value7);
  EXPECT_FALSE(value1 == value6);
}

TEST(RedexResources, StyleResourceValueMixedComparisons) {
  const uint8_t bytes_type = 1;
  const uint8_t string_type = 2;
  const uint8_t styled_type = 3;

  resources::StyleResource::Value bytes_value(bytes_type, 12345);
  resources::StyleResource::Value string_value(string_type, "test");

  std::vector<resources::StyleResource::Value::Span> spans = {{"bold", 0, 5}};
  resources::StyleResource::Value styled_value(styled_type, spans);

  resources::StyleResource::Value another_bytes_value(bytes_type, 0);
  EXPECT_FALSE(bytes_value == string_value);
  EXPECT_FALSE(bytes_value == styled_value);
  EXPECT_FALSE(string_value == styled_value);

  resources::StyleResource::Value bytes_as_string_type(string_type, 12345);
  EXPECT_FALSE(bytes_value == bytes_as_string_type);
}
