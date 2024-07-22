/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <limits>
#include <unordered_set>

#include "Debug.h"
#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Serialize.h"
#include "utils/Visitor.h"

#include "RedexMappedFile.h"
#include "RedexTest.h"

namespace {
const size_t UNSET{std::numeric_limits<size_t>::max()};
} // namespace

TEST(Visitor, AppendXmlId) {
  auto f = RedexMappedFile::open(get_env("test_manifest_path"));

  // Read some data about original file, used for asserts later.
  size_t initial_attributes{0};
  size_t initial_strings{0};
  {
    arsc::SimpleXmlParser parser;
    parser.visit((void*)f.const_data(), f.size());
    initial_attributes = parser.attribute_count();
    initial_strings = parser.global_strings().size();
  }

  // Simple cases where the API call does not need modifications.
  {
    android::Vector<char> vec;
    size_t idx{UNSET};
    auto ret = arsc::ensure_attribute_in_xml_doc(f.const_data(), f.size(),
                                                 "package", 0, &vec, &idx);
    EXPECT_EQ(ret, android::OK);
    EXPECT_TRUE(vec.empty());
    EXPECT_LT(idx, UNSET);
  }

  {
    android::Vector<char> vec;
    size_t idx{UNSET};
    auto ret = arsc::ensure_attribute_in_xml_doc(
        f.const_data(), f.size(), "enabled", 0x0101000e, &vec, &idx);
    EXPECT_EQ(ret, android::OK);
    EXPECT_TRUE(vec.empty());
    EXPECT_LT(idx, UNSET);
  }

  // An edit that is malformed, should return gracefully with an error.
  {
    android::Vector<char> vec;
    size_t idx{UNSET};
    auto ret = arsc::ensure_attribute_in_xml_doc(
        f.const_data(), f.size(), "not good", 0x0101000e, &vec, &idx);
    EXPECT_NE(ret, android::OK);
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(idx, UNSET);
  }

  // Should add 1 new item to the pool and attribute id array.
  {
    android::Vector<char> vec;
    size_t idx{UNSET};
    std::string new_attr("debuggable");
    auto ret = arsc::ensure_attribute_in_xml_doc(
        f.const_data(), f.size(), new_attr, 0x0101000f, &vec, &idx);
    EXPECT_EQ(ret, android::OK);
    EXPECT_FALSE(vec.empty());
    EXPECT_LT(idx, UNSET);

    // Make sure the resulting data looks reasonable.
    arsc::SimpleXmlParser parser;
    EXPECT_TRUE(parser.visit((void*)vec.array(), vec.size()));
    EXPECT_EQ(parser.attribute_count(), initial_attributes + 1)
        << "Attribute ID was not added!";
    auto& pool = parser.global_strings();
    EXPECT_EQ(pool.size(), initial_strings + 1) << "String was not added!";
    bool found_string = false;
    for (size_t i = 0; i < parser.attribute_count(); i++) {
      auto s = arsc::get_string_from_pool(pool, i);
      if (s == new_attr) {
        found_string = true;
        break;
      }
    }
    EXPECT_TRUE(found_string)
        << "String pool did not contain the string \"" << new_attr << "\"";

    // Actually parse it with the Android Framework class, to ensure that
    // attribute values in unrelated parts of the document are still correct.
    android::ResXMLTree xml_tree;
    xml_tree.setTo(vec.array(), vec.size());
    EXPECT_EQ(xml_tree.getError(), android::NO_ERROR)
        << "Android Framework failed to parse manifest after editing!";
    android::ResXMLParser::event_code_t event_code;
    android::String16 manifest_element("manifest");
    android::String16 package_attr("package");
    android::String16 expected_package_value("com.fb.bundles");
    bool found_attribute{false};
    do {
      event_code = xml_tree.next();
      if (event_code == android::ResXMLParser::START_TAG) {
        size_t outLen;
        auto element_name = android::String16(xml_tree.getElementName(&outLen));
        if (element_name == manifest_element) {
          const size_t attr_count = xml_tree.getAttributeCount();
          for (size_t a = 0; a < attr_count; ++a) {
            size_t len;
            android::String16 attr_name(xml_tree.getAttributeName(a, &len));
            if (attr_name == package_attr) {
              // ResXMLTree_attribute stores redundant indices to the string
              // pool, from rawValue and typedValue. Thoroughly check both.
              {
                auto chars = xml_tree.getAttributeStringValue(a, &len);
                EXPECT_NE(chars, nullptr) << "Attribute value was null";
                android::String16 attr_value(chars, len);
                EXPECT_EQ(attr_value, expected_package_value)
                    << "Attribute raw value not remapped!";
              }
              {
                // Now make sure typedValue is correct.
                auto data = xml_tree.getAttributeData(a);
                EXPECT_GE(data, 0);
                auto chars = pool.stringAt((size_t)data, &len);
                EXPECT_NE(chars, nullptr) << "Attribute value was null";
                android::String16 attr_value(chars, len);
                EXPECT_EQ(attr_value, expected_package_value)
                    << "Attribute data not remapped!";
              }
              found_attribute = true;
              break;
            }
          }
        }
      }
    } while ((event_code != android::ResXMLParser::END_DOCUMENT) &&
             (event_code != android::ResXMLParser::BAD_DOCUMENT));

    EXPECT_TRUE(found_attribute)
        << "Did not find expected <manifest> attribute";
  }
}

TEST(Visitor, AppendXmlIdUtf8Pool) {
  auto f = RedexMappedFile::open(get_env("test_views"));

  arsc::SimpleXmlParser orig_parser;
  orig_parser.visit((void*)f.const_data(), f.size());
  auto& original_strings = orig_parser.global_strings();

  android::Vector<char> vec;
  size_t idx{UNSET};
  std::string new_attr("fake");
  auto ret = arsc::ensure_attribute_in_xml_doc(f.const_data(), f.size(),
                                               new_attr, 0xf, &vec, &idx);
  EXPECT_EQ(ret, android::OK);
  EXPECT_FALSE(vec.empty());
  EXPECT_EQ(idx, 0);

  // Make sure the resulting string pool is still correct.
  arsc::SimpleXmlParser parser;
  EXPECT_TRUE(parser.visit((void*)vec.array(), vec.size()));
  auto& string_pool = parser.global_strings();

  EXPECT_EQ(string_pool.size(), original_strings.size() + 1);
  EXPECT_EQ(arsc::get_string_from_pool(string_pool, idx), new_attr);
  for (size_t i = 1; i < string_pool.size(); i++) {
    size_t a_len;
    auto a = string_pool.string8At(i, &a_len);

    size_t b_len;
    auto b = original_strings.string8At(i - 1, &b_len);

    EXPECT_EQ(a_len, b_len) << "Wrong string length at idx: " << i;
    EXPECT_EQ(strncmp(a, b, a_len), 0) << "Incorrect string data at idx: " << i;
  }
}
