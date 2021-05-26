/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include "BundleResources.h"
#include "Debug.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "RedexTestUtils.h"
#include "Trace.h"

using namespace boost::filesystem;

namespace {

void copy_file(const std::string& from, const std::string& to) {
  std::ifstream src_stream(from, std::ios::binary);
  std::ofstream dest_stream(to, std::ios::binary);
  dest_stream << src_stream.rdbuf();
}

void setup_resources_and_run(
    const std::function<void(const std::string& extract_dir, BundleResources*)>&
        callback) {
  auto tmp_dir = redex::make_tmp_dir("BundleResourcesTest%%%%%%%%");
  boost::filesystem::path p(tmp_dir.path);

  auto manifest_dir = p / "base/manifest";
  create_directories(manifest_dir);
  copy_file(std::getenv("test_manifest_path"),
            manifest_dir.string() + "/AndroidManifest.xml");

  auto layout_dir = p / "base/res/layout";
  create_directories(layout_dir);
  auto layout_dest = layout_dir.string() + "/activity_main.xml";
  copy_file(std::getenv("test_layout_path"), layout_dest);

  BundleResources resources(tmp_dir.path);
  callback(tmp_dir.path, &resources);
}

ComponentTagInfo find_component_info(const std::vector<ComponentTagInfo>& list,
                                     const std::string& classname) {
  for (const auto& info : list) {
    if (classname == info.classname) {
      return info;
    }
  }
  throw std::runtime_error("Not found: " + classname);
}
} // namespace

TEST(BundleResources, TestReadMinSdk) {
  setup_resources_and_run(
      [&](const std::string& extract_dir, BundleResources* resources) {
        auto result = resources->get_min_sdk();
        EXPECT_EQ(*result, 21);
      });
}

TEST(BundleResources, TestReadManifestClasses) {
  setup_resources_and_run([&](const std::string& extract_dir,
                              BundleResources* resources) {
    auto manifest_info = resources->get_manifest_class_info();
    auto app_classes = manifest_info.application_classes;
    EXPECT_EQ(app_classes.count("Lcom/fb/bundles/MyApplication;"), 1);
    EXPECT_EQ(app_classes.count("Lcom/fb/bundles/MyAppComponentFactory;"), 1);

    EXPECT_EQ(manifest_info.instrumentation_classes.count(
                  "Lcom/fb/bundles/MyInstrumentation;"),
              1);

    auto provider = find_component_info(manifest_info.component_tags,
                                        "Lcom/fb/bundles/MyContentProvider;");
    EXPECT_FALSE(provider.has_intent_filters);
    EXPECT_EQ(provider.is_exported, BooleanXMLAttribute::True);
    EXPECT_EQ(provider.permission, "com.fb.bundles.REALLY_SERIOUS");
    EXPECT_EQ(provider.authority_classes.size(), 2);
    EXPECT_EQ(provider.authority_classes.count("Lyo;"), 1);
    EXPECT_EQ(provider.authority_classes.count("Lsup;"), 1);

    auto receiver = find_component_info(manifest_info.component_tags,
                                        "Lcom/fb/bundles/MyReceiver;");
    EXPECT_TRUE(receiver.has_intent_filters);
    EXPECT_EQ(receiver.is_exported, BooleanXMLAttribute::True);
    EXPECT_EQ(receiver.permission, "com.fb.bundles.REALLY_SERIOUS");
    EXPECT_EQ(receiver.authority_classes.size(), 0);

    auto service = find_component_info(manifest_info.component_tags,
                                       "Lcom/fb/bundles/MyIntentService;");
    EXPECT_FALSE(service.has_intent_filters);
    EXPECT_EQ(service.is_exported, BooleanXMLAttribute::False);
    EXPECT_EQ(service.authority_classes.size(), 0);

    auto public_activity = find_component_info(
        manifest_info.component_tags, "Lcom/fb/bundles/PublicActivity;");
    EXPECT_FALSE(public_activity.has_intent_filters);
    EXPECT_EQ(public_activity.is_exported, BooleanXMLAttribute::True);
    EXPECT_EQ(public_activity.authority_classes.size(), 0);

    auto private_activity = find_component_info(
        manifest_info.component_tags, "Lcom/fb/bundles/PrivateActivity;");
    EXPECT_FALSE(private_activity.has_intent_filters);
    EXPECT_EQ(private_activity.is_exported, BooleanXMLAttribute::False);
    EXPECT_EQ(private_activity.authority_classes.size(), 0);

    auto main_activity = find_component_info(manifest_info.component_tags,
                                             "Lcom/fb/bundles/MainActivity;");
    EXPECT_TRUE(main_activity.has_intent_filters);
    EXPECT_EQ(main_activity.is_exported, BooleanXMLAttribute::Undefined);
    EXPECT_EQ(main_activity.authority_classes.size(), 0);

    bool found_alias = false;
    for (const auto& info : manifest_info.component_tags) {
      if (info.tag == ComponentTag::ActivityAlias) {
        found_alias = true;
        EXPECT_EQ(info.classname, "Lcom/fb/bundles/PublicActivity;");
      }
    }
    EXPECT_TRUE(found_alias);
  });
}

TEST(BundleResources, ReadLayout) {
  setup_resources_and_run(
      [&](const std::string& extract_dir, BundleResources* resources) {
        std::unordered_set<std::string> layout_classes;
        std::unordered_set<std::string> attrs_to_read;
        attrs_to_read.emplace(ONCLICK_ATTRIBUTE);
        std::unordered_multimap<std::string, std::string> attribute_values;
        resources->collect_layout_classes_and_attributes_for_file(
            std::getenv("test_layout_path"),
            attrs_to_read,
            &layout_classes,
            &attribute_values);
        EXPECT_EQ(layout_classes.size(), 2);
        EXPECT_EQ(layout_classes.count("Lcom/fb/bundles/WickedCoolButton;"), 1);
        EXPECT_EQ(layout_classes.count("Lcom/fb/bundles/NiftyViewGroup;"), 1);

        auto range = attribute_values.equal_range(ONCLICK_ATTRIBUTE);
        size_t found_method_names = 0;
        for (auto it = range.first; it != range.second; ++it) {
          found_method_names++;
          EXPECT_TRUE(it->second == "performFoo" || it->second == "performBar");
        }
        EXPECT_EQ(found_method_names, 2);
      });
}

TEST(BundleResources, RenameLayout) {
  setup_resources_and_run(
      [&](const std::string& extract_dir, BundleResources* resources) {
        std::map<std::string, std::string> rename_map;
        rename_map.emplace("com.fb.bundles.WickedCoolButton", "X.001");
        rename_map.emplace("com.fb.bundles.NiftyViewGroup", "X.002");
        resources->rename_classes_in_layouts(rename_map);

        // Read the file again to see it take effect
        std::unordered_set<std::string> layout_classes;
        std::unordered_set<std::string> attrs_to_read;
        std::unordered_multimap<std::string, std::string> attribute_values;
        resources->collect_layout_classes_and_attributes_for_file(
            extract_dir + "/base/res/layout/activity_main.xml",
            attrs_to_read,
            &layout_classes,
            &attribute_values);
        EXPECT_EQ(layout_classes.size(), 2);
        EXPECT_EQ(layout_classes.count("LX/001;"), 1);
        EXPECT_EQ(layout_classes.count("LX/002;"), 1);
      });
}
