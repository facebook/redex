/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexInstruction.h"
#include "Match.h"
#include "RedexResources.h"
#include "VerifyUtil.h"
#include "androidfw/ResourceTypes.h"

void verify_layout(const std::string& filename) {
  std::unordered_set<std::string> classes;
  std::unordered_set<std::string> unused_attrs;
  std::unordered_multimap<std::string, std::string> unused_attr_values;
  collect_layout_classes_and_attributes_for_file(filename, unused_attrs,
                                                 classes, unused_attr_values);
  EXPECT_EQ(classes.size(), 1)
      << "Expected 1 View in layout file: " << filename;
  auto cls_name = *classes.begin();
  EXPECT_EQ(cls_name.find("LX/"), 0)
      << "Got unexpected class name in layout: " << cls_name;
}

TEST_F(PostVerify, RenameClassesV2) {
  std::cout << "Loaded classes: " << classes.size() << std::endl;
  verify_layout(resources["res/layout/simple_layout.xml"]);
}
