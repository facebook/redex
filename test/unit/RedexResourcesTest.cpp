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
      std::getenv("test_layout_path"),
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
      std::getenv("another_layout_path"),
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
