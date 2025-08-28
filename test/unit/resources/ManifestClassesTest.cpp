/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApkResources.h"
#include "RedexTestUtils.h"

#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

// Setup a "normal" looking unpacked apk directory; creates a reader for the
// callback.
void run_with_manifest(const std::string& env_var,
                       const std::function<void(ApkResources*)>& callback) {
  auto tmp_dir = redex::make_tmp_dir("ManifestClassesTest%%%%%%%%");
  std::ifstream src_stream(std::getenv(env_var.c_str()), std::ios::binary);
  auto dest = tmp_dir.path + "/AndroidManifest.xml";
  {
    std::ofstream dest_stream(dest, std::ios::binary);
    dest_stream << src_stream.rdbuf();
  }

  ApkResources resources(tmp_dir.path);
  callback(&resources);
}

TEST(ManifestClassesTest, exported) {
  run_with_manifest("test_manifest_path", [](ApkResources* resources) {
    const auto& class_info = resources->get_manifest_class_info();

    const auto& tag_infos = class_info.component_tags;
    EXPECT_EQ(tag_infos.size(), 5);

    EXPECT_EQ(tag_infos[0].tag, ComponentTag::Activity);
    EXPECT_EQ(tag_infos[0].classname, "Ltest1;");
    EXPECT_EQ(tag_infos[0].is_exported, BooleanXMLAttribute::True);
    EXPECT_FALSE(tag_infos[0].has_intent_filters);

    EXPECT_EQ(tag_infos[1].tag, ComponentTag::Activity);
    EXPECT_EQ(tag_infos[1].classname, "Ltest2;");
    EXPECT_EQ(tag_infos[1].is_exported, BooleanXMLAttribute::Undefined);
    EXPECT_FALSE(tag_infos[1].has_intent_filters);

    EXPECT_EQ(tag_infos[2].tag, ComponentTag::Activity);
    EXPECT_EQ(tag_infos[2].classname, "Ltest3;");
    EXPECT_EQ(tag_infos[2].is_exported, BooleanXMLAttribute::Undefined);
    EXPECT_TRUE(tag_infos[2].has_intent_filters);

    EXPECT_EQ(tag_infos[3].tag, ComponentTag::Activity);
    EXPECT_EQ(tag_infos[3].classname, "Ltest4;");
    EXPECT_EQ(tag_infos[3].is_exported, BooleanXMLAttribute::False);
    EXPECT_FALSE(tag_infos[3].has_intent_filters);

    EXPECT_EQ(tag_infos[4].tag, ComponentTag::Provider);
    EXPECT_EQ(tag_infos[4].classname, "Lcom/example/x/Foo;");
    EXPECT_EQ(tag_infos[4].is_exported, BooleanXMLAttribute::Undefined);
    EXPECT_THAT(unordered_unsafe_unwrap(tag_infos[4].authority_classes),
                ::testing::UnorderedElementsAre("Lcom/example/x/Foo;",
                                                "Lcom/example/y/Bar;"));
  });
}

TEST(ManifestClassesTest, fully_qualify_class_names) {
  // This manifest file has attribute values like ".ActivityB" which need to be
  // turned into real class names.
  run_with_manifest("test_manifest_exported_path", [](ApkResources* resources) {
    const auto& class_info = resources->get_manifest_class_info();
    std::set<std::string> class_names;
    for (const auto& component : class_info.component_tags) {
      class_names.emplace(component.classname);
    }
    EXPECT_THAT(class_names,
                ::testing::ElementsAre("Lcom/fb/exportedtest/ActivityB;",
                                       "Lcom/fb/exportedtest/MainActivity;"));
  });
}
