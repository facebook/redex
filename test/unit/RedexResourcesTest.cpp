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
  std::unordered_set<std::string> attributes_to_find;
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
