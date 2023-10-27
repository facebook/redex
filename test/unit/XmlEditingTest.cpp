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

TEST(Visitor, AppendXmlId) {
  auto f = RedexMappedFile::open(std::getenv("test_manifest_path"));
  const size_t UNSET{std::numeric_limits<size_t>::max()};

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
  }
}
