/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unordered_set>

#include "BundleResources.h"
#include "Debug.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "ResourceValueMergingPass.h"
#include "ResourcesTestDefs.h"
#include "ResourcesValidationHelper.h"
#include "Trace.h"
#include "androidfw/ResourceTypes.h"

using namespace boost::filesystem;
using ::testing::Contains;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Key;
using ::testing::Not;
using ::testing::SizeIs;

namespace {

void setup_resources_and_run(
    const std::function<void(const std::string& extract_dir, BundleResources*)>&
        callback) {
  auto tmp_dir = redex::make_tmp_dir("BundleResourcesTest%%%%%%%%");
  boost::filesystem::path p(tmp_dir.path);

  auto res_dir = p / "base";
  create_directories(res_dir);
  redex::copy_file(get_env("test_res_path"),
                   res_dir.string() + "/resources.pb");

  auto manifest_dir = p / "base/manifest";
  create_directories(manifest_dir);
  redex::copy_file(get_env("test_manifest_path"),
                   manifest_dir.string() + "/AndroidManifest.xml");

  auto layout_dir = p / "base/res/layout";
  create_directories(layout_dir);
  auto layout_dest = layout_dir.string() + "/activity_main.xml";
  redex::copy_file(get_env("test_layout_path"), layout_dest);

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

void dump_string_reference_set(
    const resources::StringOrReferenceSet& layout_classes) {
  for (const auto& c : UnorderedIterable(layout_classes)) {
    if (c.is_reference()) {
      std::cerr << "LAYOUT CLASS REF: 0x" << std::hex << c.ref << std::dec
                << std::endl;
    } else {
      std::cerr << "LAYOUT CLASS: " << c.str << std::endl;
    }
  }
}

uint32_t get_resource_id(const std::string& name, BundleResources* resources) {
  auto res_table = resources->load_res_table();
  auto ids = res_table->get_res_ids_by_name(name);
  return *ids.begin();
}
} // namespace

TEST(BundleResources, TestReadManifest) {
  setup_resources_and_run(
      [&](const std::string& extract_dir, BundleResources* resources) {
        auto result = resources->get_min_sdk();
        EXPECT_EQ(*result, 21);

        auto package_name = resources->get_manifest_package_name();
        EXPECT_STREQ(package_name->c_str(), "com.fb.bundles");
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
                  "Landroidx/test/runner/AndroidJUnitRunner;"),
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

// Test collecting resource ids from xml attributes.
TEST(BundleResources, TestCollectRidsFromXmlAttrs) {
  setup_resources_and_run(
      [&](const std::string& path, BundleResources* resources) {
        auto rids = resources->get_xml_reference_attributes(
            path + "/base/manifest/AndroidManifest.xml");
        // @string/app_name, @drawable/icon and @style/ThemeA
        EXPECT_EQ(rids.size(), 3);
      });
}

// Test collecting resource ids from xml attributes.
TEST(BundleResources, TestCollectResFilesByRid) {
  setup_resources_and_run([&](const std::string& /* extract_dir */,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();

    auto icon_ids = res_table->get_res_ids_by_name("icon");
    EXPECT_EQ(icon_ids.size(), 1);
    auto files = res_table->get_files_by_rid(icon_ids[0]);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "res/drawable-mdpi-v4/icon.png");
    files = res_table->get_files_by_rid(icon_ids[0], ResourcePathType::ZipPath);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "base/res/drawable-mdpi-v4/icon.png")
        << "file path incorrect or base module not appended";

    auto prickly_ids = res_table->get_res_ids_by_name("prickly");
    EXPECT_EQ(prickly_ids.size(), 1);
    files = res_table->get_files_by_rid(prickly_ids[0]);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "res/drawable-mdpi-v4/prickly.png");

    auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
    EXPECT_EQ(padding_right_ids.size(), 1);
    files = res_table->get_files_by_rid(prickly_ids[0]);
    EXPECT_EQ(files.size(), 1);
  });
}

TEST(BundleResources, ReadLayout) {
  setup_resources_and_run([&](const std::string& extract_dir,
                              BundleResources* resources) {
    resources::StringOrReferenceSet layout_classes;
    UnorderedSet<std::string> attrs_to_read;
    attrs_to_read.emplace(ONCLICK_ATTRIBUTE);
    std::unordered_multimap<std::string, resources::StringOrReference>
        attribute_values;
    resources->collect_layout_classes_and_attributes_for_file(
        get_env("test_layout_path"),
        attrs_to_read,
        &layout_classes,
        &attribute_values);
    dump_string_reference_set(layout_classes);
    EXPECT_EQ(layout_classes.size(), 3);
    EXPECT_EQ(count_strings(layout_classes, "com.fb.bundles.WickedCoolButton"),
              1);
    EXPECT_EQ(count_strings(layout_classes, "com.fb.bundles.NiftyViewGroup"),
              1);
    auto ref_id = get_resource_id("indirection", resources);
    EXPECT_EQ(count_refs(layout_classes, ref_id), 1);

    auto method_names =
        string_values_for_key(attribute_values, "android:onClick");
    EXPECT_EQ(method_names.size(), 2);
    EXPECT_EQ(method_names.count("performBar"), 1);
    EXPECT_EQ(method_names.count("performFoo"), 1);

    // Parse another file with slightly different form.
    resources::StringOrReferenceSet more_classes;
    std::unordered_multimap<std::string, resources::StringOrReference>
        more_attribute_values;
    resources->collect_layout_classes_and_attributes_for_file(
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
  });
}

TEST(BundleResources, ReadLayoutResolveRefs) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, BundleResources* resources) {
        UnorderedSet<std::string> layout_classes;
        UnorderedSet<std::string> attrs_to_read;
        attrs_to_read.emplace(ONCLICK_ATTRIBUTE);
        std::unordered_multimap<std::string, std::string> attribute_values;
        resources->collect_layout_classes_and_attributes(
            attrs_to_read, &layout_classes, &attribute_values);

        EXPECT_EQ(layout_classes.size(), 4);
        EXPECT_EQ(attribute_values.size(), 2);

        // One reference should have been resolved to two possible classes.
        EXPECT_EQ(layout_classes.count("A"), 1);
        EXPECT_EQ(layout_classes.count("B"), 1);
        EXPECT_EQ(layout_classes.count("com.fb.bundles.WickedCoolButton"), 1);
        EXPECT_EQ(layout_classes.count("com.fb.bundles.NiftyViewGroup"), 1);
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
        resources::StringOrReferenceSet layout_classes;
        UnorderedSet<std::string> attrs_to_read;
        std::unordered_multimap<std::string, resources::StringOrReference>
            attribute_values;
        resources->collect_layout_classes_and_attributes_for_file(
            extract_dir + "/base/res/layout/activity_main.xml",
            attrs_to_read,
            &layout_classes,
            &attribute_values);
        dump_string_reference_set(layout_classes);
        EXPECT_EQ(layout_classes.size(), 3);
        EXPECT_EQ(count_strings(layout_classes, "X.001"), 1);
        EXPECT_EQ(count_strings(layout_classes, "X.002"), 1);
        auto ref_id = get_resource_id("indirection", resources);
        EXPECT_EQ(count_refs(layout_classes, ref_id), 1);
      });
}

TEST(BundleResources, ReadResource) {
  setup_resources_and_run([&](const std::string& /* extract_dir */,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();
    auto padding_left_ids = res_table->get_res_ids_by_name("padding_left");
    EXPECT_EQ(padding_left_ids.size(), 1);
    auto module_name =
        reinterpret_cast<ResourcesPbFile*>(res_table.get())
            ->resolve_module_name_for_resource_id(padding_left_ids[0]);
    EXPECT_STREQ("base", module_name.c_str());
    const auto& id_to_name = res_table->id_to_name;
    auto obtain_resource_name_back = id_to_name.at(padding_left_ids[0]);
    EXPECT_EQ(obtain_resource_name_back, "padding_left");
    auto bg_grey = res_table->get_res_ids_by_name("bg_grey");
    EXPECT_EQ(bg_grey.size(), 1);
    obtain_resource_name_back = id_to_name.at(bg_grey[0]);
    EXPECT_EQ(obtain_resource_name_back, "bg_grey");
    UnorderedSet<std::string> types = {"drawable"};
    auto drawable_type_id = res_table->get_types_by_name(types);
    EXPECT_EQ(drawable_type_id.size(), 1);
    std::unordered_set<std::string> drawable_res_names;
    for (const auto& pair : id_to_name) {
      auto id = pair.first;
      if (drawable_type_id.count(id & TYPE_MASK_BIT)) {
        drawable_res_names.emplace(pair.second);
      }
    }
    EXPECT_EQ(drawable_res_names.size(), 4);
    EXPECT_EQ(drawable_res_names.count("icon"), 1);
    EXPECT_EQ(drawable_res_names.count("prickly"), 1);
    EXPECT_EQ(drawable_res_names.count("x_icon"), 1);
    EXPECT_EQ(drawable_res_names.count("x_prickly"), 1);

    auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
    EXPECT_EQ(padding_right_ids.size(), 1);
    EXPECT_EQ(res_table->resource_value_identical(padding_left_ids[0],
                                                  padding_right_ids[0]),
              true);

    auto unused_dimen_2_ids = res_table->get_res_ids_by_name("unused_dimen_2");
    EXPECT_EQ(unused_dimen_2_ids.size(), 1);
    EXPECT_EQ(res_table->resource_value_identical(padding_left_ids[0],
                                                  unused_dimen_2_ids[0]),
              true);

    auto margin_top_ids = res_table->get_res_ids_by_name("margin_top");
    EXPECT_EQ(margin_top_ids.size(), 1);
    EXPECT_EQ(res_table->resource_value_identical(padding_left_ids[0],
                                                  margin_top_ids[0]),
              false);

    auto prickly_ids = res_table->get_res_ids_by_name("prickly");
    EXPECT_EQ(prickly_ids.size(), 1);
    EXPECT_EQ(res_table->resource_value_identical(padding_left_ids[0],
                                                  prickly_ids[0]),
              false);

    auto foo_ids = res_table->get_res_ids_by_name("foo");
    EXPECT_EQ(foo_ids.size(), 1);
    auto bar_ids = res_table->get_res_ids_by_name("bar");
    EXPECT_EQ(bar_ids.size(), 1);
    auto far_ids = res_table->get_res_ids_by_name("far");
    EXPECT_EQ(far_ids.size(), 1);
    auto baz_ids = res_table->get_res_ids_by_name("baz");
    EXPECT_EQ(baz_ids.size(), 1);
    auto boo_ids = res_table->get_res_ids_by_name("boo");
    EXPECT_EQ(boo_ids.size(), 1);

    EXPECT_EQ(res_table->resource_value_identical(foo_ids[0], bar_ids[0]),
              true);
    EXPECT_EQ(res_table->resource_value_identical(bar_ids[0], far_ids[0]),
              false);
    EXPECT_EQ(res_table->resource_value_identical(baz_ids[0], boo_ids[0]),
              false);

    const auto& res_table_pb = static_cast<ResourcesPbFile*>(res_table.get());
    const auto& id_to_configvalue = res_table_pb->get_res_id_to_configvalue();
    EXPECT_EQ(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(padding_left_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(padding_right_ids[0])));
    EXPECT_EQ(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(padding_left_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(unused_dimen_2_ids[0])));
    EXPECT_NE(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(padding_left_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(margin_top_ids[0])));
    EXPECT_NE(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(padding_left_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(prickly_ids[0])));
    EXPECT_EQ(
        res_table_pb->get_hash_from_values(id_to_configvalue.at(foo_ids[0])),
        res_table_pb->get_hash_from_values(id_to_configvalue.at(bar_ids[0])));
    EXPECT_NE(
        res_table_pb->get_hash_from_values(id_to_configvalue.at(far_ids[0])),
        res_table_pb->get_hash_from_values(id_to_configvalue.at(bar_ids[0])));
    EXPECT_NE(
        res_table_pb->get_hash_from_values(id_to_configvalue.at(baz_ids[0])),
        res_table_pb->get_hash_from_values(id_to_configvalue.at(boo_ids[0])));

    auto style_not_sorted_ids =
        res_table->get_res_ids_by_name("StyleNotSorted");
    EXPECT_EQ(style_not_sorted_ids.size(), 1);
    auto style_sorted_ids = res_table->get_res_ids_by_name("StyleSorted");
    EXPECT_EQ(style_sorted_ids.size(), 1);
    EXPECT_EQ(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(style_not_sorted_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(style_sorted_ids[0])));

    auto theme_different_a_ids =
        res_table->get_res_ids_by_name("ThemeDifferentA");
    EXPECT_EQ(theme_different_a_ids.size(), 1);
    auto theme_different_b_ids =
        res_table->get_res_ids_by_name("ThemeDifferentB");
    EXPECT_EQ(theme_different_b_ids.size(), 1);
    EXPECT_NE(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(theme_different_a_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(theme_different_b_ids[0])));

    auto same_attribute_a_ids =
        res_table->get_res_ids_by_name("SameAttributeA");
    EXPECT_EQ(same_attribute_a_ids.size(), 1);
    auto same_attribute_b_ids =
        res_table->get_res_ids_by_name("SameAttributeB");
    EXPECT_EQ(same_attribute_b_ids.size(), 1);
    EXPECT_EQ(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(same_attribute_a_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(same_attribute_b_ids[0])));

    auto same_styleable_a_ids =
        res_table->get_res_ids_by_name("SameStyleableA");
    EXPECT_EQ(same_styleable_a_ids.size(), 1);
    auto same_styleable_b_ids =
        res_table->get_res_ids_by_name("SameStyleableB");
    EXPECT_EQ(same_styleable_b_ids.size(), 1);
    EXPECT_NE(res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(same_styleable_a_ids[0])),
              res_table_pb->get_hash_from_values(
                  id_to_configvalue.at(same_styleable_b_ids[0])));
  });
}

TEST(BundleResources, WriteResource) {
  setup_resources_and_run([&](const std::string& extract_dir,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();
    auto padding_left_ids = res_table->get_res_ids_by_name("padding_left");
    EXPECT_EQ(padding_left_ids.size(), 1);
    auto padding_left_id = padding_left_ids[0];
    auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
    EXPECT_EQ(padding_right_ids.size(), 1);
    auto padding_right_id = padding_right_ids[0];
    auto unused_dimen_1_ids = res_table->get_res_ids_by_name("unused_dimen_1");
    EXPECT_EQ(unused_dimen_1_ids.size(), 1);
    auto unused_dimen_1_id = unused_dimen_1_ids[0];
    auto unused_dimen_2_ids = res_table->get_res_ids_by_name("unused_dimen_2");
    EXPECT_EQ(unused_dimen_2_ids.size(), 1);
    auto unused_dimen_2_id = unused_dimen_2_ids[0];

    res_table->delete_resource(unused_dimen_1_id);
    res_table->delete_resource(unused_dimen_2_id);
    std::map<uint32_t, uint32_t> to_replace;
    to_replace[padding_left_id] = unused_dimen_1_id;
    to_replace[padding_right_id] = unused_dimen_2_id;

    res_table->remap_res_ids_and_serialize({extract_dir + "/base/resources.pb"},
                                           to_replace);
    auto res_table_new = resources->load_res_table();

    EXPECT_EQ(res_table_new->get_res_ids_by_name("unused_dimen_2").size(), 0);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("unused_dimen_1").size(), 0);
    padding_left_ids = res_table_new->get_res_ids_by_name("padding_left");
    EXPECT_EQ(padding_left_ids.size(), 1);
    EXPECT_EQ(padding_left_ids[0], unused_dimen_1_id);
    padding_right_ids = res_table_new->get_res_ids_by_name("padding_right");
    EXPECT_EQ(padding_right_ids.size(), 1);
    EXPECT_EQ(padding_right_ids[0], unused_dimen_2_id);
  });
}

TEST(BundleResources, ChangeResourceIdInLayout) {
  setup_resources_and_run([&](const std::string& extract_dir,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();
    auto margin_top_ids = res_table->get_res_ids_by_name("margin_top");
    EXPECT_EQ(margin_top_ids.size(), 1);
    auto margin_top_id = margin_top_ids[0];
    auto padding_right_ids = res_table->get_res_ids_by_name("padding_right");
    EXPECT_EQ(padding_right_ids.size(), 1);
    auto padding_right_id = padding_right_ids[0];
    auto prickly_ids = res_table->get_res_ids_by_name("prickly");
    EXPECT_EQ(prickly_ids.size(), 1);
    auto prickly_id = prickly_ids[0];
    auto icon_ids = res_table->get_res_ids_by_name("icon");
    EXPECT_EQ(icon_ids.size(), 1);
    auto icon_id = icon_ids[0];
    std::map<uint32_t, uint32_t> kept_to_remapped_ids;
    kept_to_remapped_ids[prickly_id] = icon_id;
    kept_to_remapped_ids[margin_top_id] = padding_right_id;
    auto changed = resources->remap_xml_reference_attributes(
        extract_dir + "/base/res/layout/activity_main.xml",
        kept_to_remapped_ids);
    EXPECT_EQ(changed, 4);
  });
}

TEST(BundleResources, ObfuscateResourcesName) {
  setup_resources_and_run([&](const std::string& /* unused */,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();
    auto color1_ids = res_table->get_res_ids_by_name("bg_grey");
    EXPECT_EQ(color1_ids.size(), 1);
    auto color1_id = color1_ids[0];
    auto color2_ids = res_table->get_res_ids_by_name("keep_me_unused_color");
    EXPECT_EQ(color2_ids.size(), 1);
    auto color2_id = color2_ids[0];
    auto color3_ids = res_table->get_res_ids_by_name("prickly_green");
    EXPECT_EQ(color3_ids.size(), 1);
    auto color3_id = color3_ids[0];
    auto hex_or_file2_ids = res_table->get_res_ids_by_name("hex_or_file2");
    EXPECT_EQ(hex_or_file2_ids.size(), 1);
    auto hex_or_file2_id = hex_or_file2_ids[0];
    auto hex_or_file_ids = res_table->get_res_ids_by_name("hex_or_file");
    EXPECT_EQ(hex_or_file_ids.size(), 1);
    auto hex_or_file_id = hex_or_file_ids[0];
    auto duplicate_name_ids = res_table->get_res_ids_by_name("duplicate_name");
    EXPECT_EQ(duplicate_name_ids.size(), 3);
    auto dimen1_ids = res_table->get_res_ids_by_name("unused_dimen_2");
    EXPECT_EQ(dimen1_ids.size(), 1);
    auto dimen1_id = dimen1_ids[0];

    auto icon_ids = res_table->get_res_ids_by_name("icon");
    EXPECT_EQ(icon_ids.size(), 1);
    auto files = res_table->get_files_by_rid(icon_ids[0]);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "res/drawable-mdpi-v4/icon.png");
    files = res_table->get_files_by_rid(icon_ids[0], ResourcePathType::ZipPath);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "base/res/drawable-mdpi-v4/icon.png");

    UnorderedSet<std::string> types = {"color"};
    auto type_ids = res_table->get_types_by_name(types);
    UnorderedSet<uint32_t> shifted_allow_type_ids;
    for (auto& type_id : UnorderedIterable(type_ids)) {
      shifted_allow_type_ids.emplace(type_id >> TYPE_INDEX_BIT_SHIFT);
    }
    std::map<std::string, std::string> filepath_old_to_new;
    filepath_old_to_new["base/res/drawable-mdpi-v4/icon.png"] =
        "base/res/a.png";
    res_table->obfuscate_resource_and_serialize(
        resources->find_resources_files(), filepath_old_to_new,
        shifted_allow_type_ids, {"keep_me_unused_"}, {});

    auto res_table_new = resources->load_res_table();

    EXPECT_EQ(res_table_new->get_res_ids_by_name("bg_grey").size(), 0);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("prickly_green").size(), 0);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("keep_me_unused_color").size(),
              1);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("unused_dimen_2").size(), 1);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("hex_or_file").size(), 0);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("hex_or_file2").size(), 0);
    EXPECT_EQ(res_table_new->get_res_ids_by_name("duplicate_name").size(), 2);
    EXPECT_EQ(res_table_new->get_res_ids_by_name(RESOURCE_NAME_REMOVED).size(),
              7);
    const auto& id_to_name = res_table_new->id_to_name;
    EXPECT_EQ(id_to_name.at(color1_id), RESOURCE_NAME_REMOVED);
    EXPECT_EQ(id_to_name.at(color3_id), RESOURCE_NAME_REMOVED);
    EXPECT_EQ(id_to_name.at(hex_or_file2_id), RESOURCE_NAME_REMOVED);
    EXPECT_EQ(id_to_name.at(hex_or_file_id), RESOURCE_NAME_REMOVED);
    EXPECT_EQ(id_to_name.at(color2_id), "keep_me_unused_color");
    EXPECT_EQ(id_to_name.at(dimen1_id), "unused_dimen_2");

    icon_ids = res_table_new->get_res_ids_by_name("icon");
    EXPECT_EQ(icon_ids.size(), 1);
    files = res_table_new->get_files_by_rid(icon_ids[0]);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "res/a.png");
    files =
        res_table_new->get_files_by_rid(icon_ids[0], ResourcePathType::ZipPath);
    EXPECT_EQ(files.size(), 1);
    EXPECT_EQ(*files.begin(), "base/res/a.png");
  });
}

TEST(BundleResources, GetConfigurations) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, BundleResources* resources) {
        auto res_table = resources->load_res_table();
        EXPECT_EQ(res_table->package_count(), 1);
        std::vector<android::ResTable_config> configs;
        res_table->get_configurations(APPLICATION_PACKAGE, "color", &configs);
        EXPECT_EQ(configs.size(), 2);
        EXPECT_STREQ(configs[0].toString().c_str(), "");
        EXPECT_STREQ(configs[1].toString().c_str(), "night");
        configs.clear();
        res_table->get_configurations(APPLICATION_PACKAGE, "dimen", &configs);
        EXPECT_EQ(configs.size(), 2);
        EXPECT_STREQ(configs[0].toString().c_str(), "");
        EXPECT_STREQ(configs[1].toString().c_str(), "land");
        configs.clear();
        res_table->get_configurations(APPLICATION_PACKAGE, "nope", &configs);
        EXPECT_EQ(configs.size(), 0);
      });
}

TEST(BundleResources, GetConfigsWithValue) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, BundleResources* resources) {
        auto res_table = resources->load_res_table();
        EXPECT_EQ(res_table->package_count(), 1);
        auto config_set = res_table->get_configs_with_values(0x7f04000f);
        EXPECT_EQ(config_set.size(), 1);
        EXPECT_STREQ(config_set.begin()->toString().c_str(), "land");

        auto another_set = res_table->get_configs_with_values(0x7f030002);
        EXPECT_EQ(another_set.size(), 2);
        auto it = another_set.begin();
        EXPECT_STREQ(it->toString().c_str(), "");
        it++;
        EXPECT_STREQ(it->toString().c_str(), "night");
      });
}

TEST(BundleResources, GetOverlayableRootIds) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, BundleResources* resources) {
        auto res_table = resources->load_res_table();
        // Check the correct ids are returned as roots.
        auto overlayables = res_table->get_overlayable_id_roots();
        EXPECT_EQ(overlayables.size(),
                  sample_app::EXPECTED_OVERLAYABLE_RESOURCES.size());
        for (auto& name : sample_app::EXPECTED_OVERLAYABLE_RESOURCES) {
          EXPECT_TRUE(is_overlayable(name, res_table.get()))
              << name << " is not overlayable!";
        }
      });
}

TEST(BundleResources, TestNames) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, BundleResources* resources) {
        auto res_table = resources->load_res_table();
        EXPECT_TRUE(res_table->is_type_named(0x1, "array"));
        EXPECT_TRUE(res_table->is_type_named(0x2, "attr"));
      });
}

TEST(BundleResources, WalkReferences) {
  setup_resources_and_run(
      [&](const std::string& /* unused */, BundleResources* resources) {
        auto res_table = resources->load_res_table();
        validate_walk_references_for_resource(res_table.get());
      });
}

TEST(BundleResources, TestRemoveStyleAttribute) {
  setup_resources_and_run([&](const std::string& /* unused */,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();
    const auto& paths = resources->find_resources_files();
    auto style_map = res_table->get_style_map();

    std::vector<std::string> theme_names = {"CustomText.Prickly",
                                            "CustomText.Unused", "CustomText",
                                            "ChooseMe", "ChildWithParentAttr"};
    std::vector<resources::StyleModificationSpec::Modification> modifications;
    UnorderedMap<uint32_t, UnorderedSet<uint32_t>> original_attributes;
    UnorderedMap<uint32_t, uint32_t> style_id_to_remove_attr;

    for (const auto& theme_name : theme_names) {
      auto style_ids = res_table->get_res_ids_by_name(theme_name);
      EXPECT_THAT(style_ids, SizeIs(1));
      uint32_t style_id = style_ids[0];

      EXPECT_THAT(style_map, Contains(Key(style_id)))
          << "Style ID 0x" << std::hex << style_id << std::dec
          << " not found in style map";

      const std::vector<resources::StyleResource>& style_resources =
          style_map[style_id];
      EXPECT_THAT(style_resources, Not(IsEmpty()))
          << "No style resources found for " << theme_name;

      for (const auto& attr_pair : style_resources[0].attributes) {
        original_attributes[style_id].insert(attr_pair.first);
      }

      uint32_t attr_id = style_resources[0].attributes.begin()->first;
      EXPECT_THAT(attr_id, Not(Eq(0)))
          << "No attributes found in the style " << theme_name;

      style_id_to_remove_attr[style_id] = attr_id;
      modifications.push_back(
          resources::StyleModificationSpec::Modification(style_id, attr_id));
    }

    res_table->apply_attribute_removals_and_additions(modifications, paths);

    auto new_res_table = resources->load_res_table();
    resources::StyleMap updated_style_map = new_res_table->get_style_map();

    for (const auto& mod : modifications) {
      uint32_t resource_id = mod.resource_id;
      uint32_t attr_id = mod.attribute_id.value();

      EXPECT_THAT(updated_style_map, Contains(Key(resource_id)))
          << "Style ID 0x" << std::hex << resource_id << std::dec
          << " not found in style map";

      const std::vector<resources::StyleResource>& new_style_resources =
          updated_style_map[resource_id];
      EXPECT_THAT(new_style_resources, SizeIs(1));

      EXPECT_THAT(new_style_resources[0].attributes,
                  Not(Contains(Key(attr_id))))
          << "Attribute 0x" << std::hex << attr_id << std::dec
          << " was not removed from style 0x" << std::hex << resource_id;

      UnorderedSet<uint32_t> expected_attributes =
          original_attributes[resource_id];
      expected_attributes.erase(attr_id);

      UnorderedSet<uint32_t> actual_attributes;
      for (const auto& attr_pair : new_style_resources[0].attributes) {
        actual_attributes.insert(attr_pair.first);
      }

      EXPECT_THAT(actual_attributes, Eq(expected_attributes))
          << "Attributes after removal don't match expected set for style 0x"
          << std::hex << resource_id << std::dec;
    }
  });
}

TEST(BundleResources, TestAddStyleAttribute) {
  setup_resources_and_run([&](const std::string&, BundleResources* resources) {
    using Value = resources::StyleResource::Value;
    auto res_table = resources->load_res_table();
    const auto& paths = resources->find_resources_files();
    auto style_map = res_table->get_style_map();

    std::vector<resources::StyleModificationSpec::Modification> modifications;
    UnorderedMap<uint32_t, UnorderedSet<uint32_t>> original_attributes;

    struct StyleModData {
      std::string theme_name;
      uint32_t attr_id;
      Value attr_value;

      StyleModData(const std::string& name, uint32_t id, Value&& value)
          : theme_name(name), attr_id(id), attr_value(std::move(value)) {}
    };

    const std::vector<StyleModData> style_modifications = {
        {"CustomText.Prickly", kEnabledAttrId,
         Value(android::Res_value::TYPE_INT_BOOLEAN, true)},
        {"CustomText.Unused", kTextStyleAttrId,
         Value(android::Res_value::TYPE_STRING,
               std::string("Test String Value"))},
        {"CustomText", kTextColorAttrId,
         Value(android::Res_value::TYPE_REFERENCE, 0x7f030001)},
        {"ChooseMe", kBackgroundAttrId,
         Value(android::Res_value::TYPE_INT_COLOR_ARGB8, 0xFF0000FF)},
        {"ChildWithParentAttr", kTextSize,
         Value(android::Res_value::TYPE_INT_COLOR_ARGB8, 0xFFFF0000)},
        {"CustomText.Prickly", kFloatAttrId,
         Value(android::Res_value::TYPE_FLOAT, 0x3F800000)},
        {"CustomText.Unused", kDimensionAttrId,
         Value(android::Res_value::TYPE_DIMENSION, 0x00000064)},
        {"CustomText", kFractionAttrId,
         Value(android::Res_value::TYPE_FRACTION, 0x00000032)}};

    for (const auto& mod : style_modifications) {
      auto style_ids = res_table->get_res_ids_by_name(mod.theme_name);
      EXPECT_THAT(style_ids, SizeIs(1));
      uint32_t style_id = style_ids[0];

      modifications.push_back(resources::StyleModificationSpec::Modification(
          style_id, mod.attr_id, mod.attr_value));
    }

    res_table->apply_attribute_removals_and_additions(modifications, paths);

    auto new_res_table = resources->load_res_table();
    resources::StyleMap updated_style_map = new_res_table->get_style_map();

    for (const auto& mod : modifications) {
      uint32_t resource_id = mod.resource_id;
      uint32_t attr_id = mod.attribute_id.value();

      EXPECT_THAT(updated_style_map, Contains(Key(resource_id)))
          << "Style ID 0x" << std::hex << resource_id << std::dec
          << " not found in style map";

      const std::vector<resources::StyleResource>& new_style_resources =
          updated_style_map[resource_id];
      EXPECT_THAT(new_style_resources, Not(IsEmpty()))
          << "No style resources found for resource ID 0x" << std::hex
          << resource_id;

      EXPECT_THAT(new_style_resources[0].attributes, Contains(Key(attr_id)))
          << "Attribute 0x" << std::hex << attr_id << std::dec
          << " was not added to style 0x" << std::hex << resource_id;

      const auto& added_attr = new_style_resources[0].attributes.at(attr_id);

      if (attr_id == kEnabledAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_INT_BOOLEAN);
        EXPECT_TRUE(added_attr.get_value_bytes() != 0);
      }

      if (attr_id == kTextStyleAttrId) {
        EXPECT_EQ(added_attr.get_data_type(), android::Res_value::TYPE_STRING);
        EXPECT_TRUE(added_attr.get_value_string().has_value());
        EXPECT_EQ(added_attr.get_value_string().value(), "Test String Value");
      } else if (attr_id == kTextColorAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_REFERENCE);
        EXPECT_EQ(added_attr.get_value_bytes(), 0x7f030001);
      } else if (attr_id == kBackgroundAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_INT_COLOR_ARGB8);
        EXPECT_EQ(added_attr.get_value_bytes(), 0xFF0000FF);
      } else if (attr_id == kTextSize) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_INT_COLOR_ARGB8);
        EXPECT_EQ(added_attr.get_value_bytes(), 0xFFFF0000);
      } else if (attr_id == kFloatAttrId) {
        EXPECT_EQ(added_attr.get_data_type(), android::Res_value::TYPE_FLOAT);
        EXPECT_EQ(added_attr.get_value_bytes(), 0x3F800000);
      } else if (attr_id == kDimensionAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_DIMENSION);
        EXPECT_EQ(added_attr.get_value_bytes(), 0x00000064);
      } else if (attr_id == kFractionAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_FRACTION);
        EXPECT_EQ(added_attr.get_value_bytes(), 0x00000032);
      }

      for (uint32_t original_attr_id :
           UnorderedIterable(original_attributes[resource_id])) {
        EXPECT_THAT(new_style_resources[0].attributes,
                    Contains(Key(original_attr_id)))
            << "Original attribute 0x" << std::hex << original_attr_id
            << std::dec << " is missing from style 0x" << std::hex
            << resource_id;
      }
    }
  });
}

TEST(BundleResources, TestRemoveAndAddStyleAttributes) {
  setup_resources_and_run([&](const std::string& /* unused */,
                              BundleResources* resources) {
    using Value = resources::StyleResource::Value;
    auto res_table = resources->load_res_table();
    const auto& paths = resources->find_resources_files();
    auto style_map = res_table->get_style_map();

    std::vector<resources::StyleModificationSpec::Modification> modifications;
    UnorderedMap<uint32_t, UnorderedSet<uint32_t>> original_attributes;

    struct TestData {
      std::string style_name;
      uint32_t remove_attr_id;
      uint32_t add_attr_id;
      Value add_value;
    };

    auto get_style_id = [&](const std::string& name) {
      auto ids = res_table->get_res_ids_by_name(name);
      EXPECT_THAT(ids, SizeIs(1));
      return ids[0];
    };

    auto get_first_attr = [&](uint32_t style_id) {
      return style_map[style_id][0].attributes.begin()->first;
    };

    uint32_t prickly_id = get_style_id("CustomText.Prickly");
    uint32_t unused_id = get_style_id("CustomText.Unused");
    uint32_t custom_id = get_style_id("CustomText");

    const std::vector<TestData> test_data = {
        {"CustomText.Prickly", get_first_attr(prickly_id), kEnabledAttrId,
         Value(android::Res_value::TYPE_INT_BOOLEAN, true)},
        {"CustomText.Unused", get_first_attr(unused_id), kTextStyleAttrId,
         Value(android::Res_value::TYPE_STRING, std::string("New String"))},
        {"CustomText", get_first_attr(custom_id), kTextColorAttrId,
         Value(android::Res_value::TYPE_REFERENCE, 0x7f030002)}};

    for (const auto& [style_id, style_resources] : style_map) {
      if (!style_resources.empty()) {
        for (const auto& [attr_id, _] : style_resources[0].attributes) {
          original_attributes[style_id].insert(attr_id);
        }
      }
    }

    for (const auto& data : test_data) {
      uint32_t style_id = get_style_id(data.style_name);
      modifications.push_back(resources::StyleModificationSpec::Modification(
          style_id, data.remove_attr_id));
      modifications.push_back(resources::StyleModificationSpec::Modification(
          style_id, data.add_attr_id, data.add_value));
    }

    res_table->apply_attribute_removals_and_additions(modifications, paths);

    auto new_res_table = resources->load_res_table();
    auto updated_style_map = new_res_table->get_style_map();

    for (const auto& data : test_data) {
      uint32_t style_id = get_style_id(data.style_name);
      const auto& attributes = updated_style_map[style_id][0].attributes;

      EXPECT_THAT(attributes, Not(Contains(Key(data.remove_attr_id))))
          << "Attribute not removed from " << data.style_name;
      EXPECT_THAT(attributes, Contains(Key(data.add_attr_id)))
          << "Attribute not added to " << data.style_name;

      const auto& added_attr = attributes.at(data.add_attr_id);
      if (data.add_attr_id == kEnabledAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_INT_BOOLEAN);
        EXPECT_TRUE(added_attr.get_value_bytes() != 0);
      } else if (data.add_attr_id == kTextStyleAttrId) {
        EXPECT_EQ(added_attr.get_data_type(), android::Res_value::TYPE_STRING);
        EXPECT_EQ(added_attr.get_value_string().value(), "New String");
      } else if (data.add_attr_id == kTextColorAttrId) {
        EXPECT_EQ(added_attr.get_data_type(),
                  android::Res_value::TYPE_REFERENCE);
        EXPECT_EQ(added_attr.get_value_bytes(), 0x7f030002);
      }

      UnorderedSet<uint32_t> expected_attrs = original_attributes[style_id];
      expected_attrs.erase(data.remove_attr_id);
      expected_attrs.insert(data.add_attr_id);

      UnorderedSet<uint32_t> actual_attrs;
      for (const auto& [attr_id, _] : attributes) {
        actual_attrs.insert(attr_id);
      }

      EXPECT_THAT(actual_attrs, Eq(expected_attrs))
          << "Attribute set mismatch for " << data.style_name;
    }
  });
}

TEST(BundleResources, TestResourceExists) {
  setup_resources_and_run([&](const std::string&, BundleResources* resources) {
    auto directory = resources->get_directory();
    auto res_pb_file_path = directory + "/base/resources.pb";
    auto res_table = resources->load_res_table();

    {
      UnorderedSet<uint32_t> resource_ids;
      std::vector<std::string> names = {"ChooseMe", "ParentWithAttr",
                                        "IDontExist"};
      for (const auto& name : names) {
        auto ids = res_table->get_res_ids_by_name(name);
        if (ids.empty()) {
          resource_ids.insert(0x0);
        } else {
          resource_ids.insert(ids[0]);
        }
      }
      EXPECT_ANY_THROW(
          assert_resources_in_one_file(resource_ids, {res_pb_file_path}));
    }

    {
      UnorderedSet<uint32_t> resource_ids;
      std::vector<std::string> names = {
          "ChooseMe",
          "ParentWithAttr",
      };
      for (const auto& name : names) {
        auto ids = res_table->get_res_ids_by_name(name);
        EXPECT_THAT(ids, SizeIs(1));
        resource_ids.insert(ids[0]);
      }

      assert_resources_in_one_file(resource_ids, {res_pb_file_path});
    }
  });
}
void verify_attributes(const resources::StyleResource::AttrMap& attributes,
                       const std::vector<uint32_t>& expected) {
  for (uint32_t attr_id : expected) {
    EXPECT_THAT(attributes, Contains(Key(attr_id)))
        << "Attribute 0x" << std::hex << attr_id << std::dec
        << " was not found in style";
  }
  EXPECT_EQ(attributes.size(), expected.size());
}

TEST(BundleResources, TestApplyStyleMerges) {
  setup_resources_and_run([&](const std::string& /* unused */,
                              BundleResources* resources) {
    ResourceValueMergingPass pass;

    auto res_table = resources->load_res_table();
    const auto& paths = resources->find_resources_files();
    auto style_info = res_table->load_style_info();

    auto app_theme_light_id =
        res_table->get_res_ids_by_name("AppTheme.Light").at(0);
    auto app_theme_light_blue_id =
        res_table->get_res_ids_by_name("AppTheme.Light.Blue").at(0);
    auto app_theme_light_blue_no_action_bar_id =
        res_table->get_res_ids_by_name("AppTheme.Light.Blue.NoActionBar").at(0);

    const std::vector<uint32_t> light_attributes = {kTextColorAttrId,
                                                    kBackgroundAttrId};
    const std::vector<uint32_t> blue_attributes = {kColorPrimaryAttrId,
                                                   kColorAccent};
    const std::vector<uint32_t> no_action_bar_attributes = {kWindowNoTitle,
                                                            kWindowActionBar};

    // First merge: AppTheme.Light into its children
    std::vector<uint32_t> resources_to_merge = {app_theme_light_id};
    auto modifications =
        pass.get_parent_and_attribute_modifications_for_merging(
            style_info, resources_to_merge);

    res_table->apply_style_merges({modifications}, paths);

    res_table = resources->load_res_table();
    style_info = res_table->load_style_info();

    // Verify that AppTheme.Light.Blue now contains both its own attributes and
    // Light's attributes
    auto blue_style =
        res_table->get_style_map().at(app_theme_light_blue_id).at(0);

    std::vector<uint32_t> expected_blue_attributes = blue_attributes;
    expected_blue_attributes.insert(expected_blue_attributes.end(),
                                    light_attributes.begin(),
                                    light_attributes.end());

    verify_attributes(blue_style.attributes, expected_blue_attributes);

    // Second merge: AppTheme.Light.Blue into its children
    resources_to_merge = {app_theme_light_blue_id};
    modifications = pass.get_parent_and_attribute_modifications_for_merging(
        style_info, resources_to_merge);

    res_table->apply_style_merges({modifications}, paths);

    // Verify that NoActionBar now contains all attributes from the hierarchy
    auto no_action_bar_style = resources->load_res_table()
                                   ->get_style_map()
                                   .at(app_theme_light_blue_no_action_bar_id)
                                   .at(0);

    std::vector<uint32_t> expected_no_action_bar_attributes =
        no_action_bar_attributes;
    expected_no_action_bar_attributes.insert(
        expected_no_action_bar_attributes.end(),
        blue_attributes.begin(),
        blue_attributes.end());
    expected_no_action_bar_attributes.insert(
        expected_no_action_bar_attributes.end(),
        light_attributes.begin(),
        light_attributes.end());
    verify_attributes(no_action_bar_style.attributes,
                      expected_no_action_bar_attributes);
  });
}

TEST(BundleResources, TestApplyStyleChained) {
  setup_resources_and_run([&](const std::string& /* unused */,
                              BundleResources* resources) {
    ResourceValueMergingPass pass;

    auto res_table = resources->load_res_table();
    const auto& paths = resources->find_resources_files();
    auto style_info = res_table->load_style_info();

    auto app_theme_light_id =
        res_table->get_res_ids_by_name("AppTheme.Light").at(0);
    auto app_theme_light_blue_id =
        res_table->get_res_ids_by_name("AppTheme.Light.Blue").at(0);
    auto app_theme_light_blue_no_action_bar_id =
        res_table->get_res_ids_by_name("AppTheme.Light.Blue.NoActionBar").at(0);

    const std::vector<uint32_t> light_attributes = {kTextColorAttrId,
                                                    kBackgroundAttrId};
    const std::vector<uint32_t> blue_attributes = {kColorPrimaryAttrId,
                                                   kColorAccent};
    const std::vector<uint32_t> no_action_bar_attributes = {kWindowNoTitle,
                                                            kWindowActionBar};

    // Merge both Light and Light.Blue in one operation
    std::vector<uint32_t> resources_to_merge = {app_theme_light_id,
                                                app_theme_light_blue_id};
    auto modifications =
        pass.get_parent_and_attribute_modifications_for_merging(
            style_info, resources_to_merge);

    res_table->apply_style_merges({modifications}, paths);

    res_table = resources->load_res_table();
    style_info = res_table->load_style_info();

    // Verify that NoActionBar now contains all attributes from the hierarchy
    auto no_action_bar_style = res_table->get_style_map()
                                   .at(app_theme_light_blue_no_action_bar_id)
                                   .at(0);

    std::vector<uint32_t> expected_no_action_bar_attributes =
        no_action_bar_attributes;
    expected_no_action_bar_attributes.insert(
        expected_no_action_bar_attributes.end(),
        blue_attributes.begin(),
        blue_attributes.end());
    expected_no_action_bar_attributes.insert(
        expected_no_action_bar_attributes.end(),
        light_attributes.begin(),
        light_attributes.end());
    verify_attributes(no_action_bar_style.attributes,
                      expected_no_action_bar_attributes);
  });
}

TEST(BundleResources, TestAddStyles) {
  setup_resources_and_run([&](const std::string& /* unused */,
                              BundleResources* resources) {
    auto res_table = resources->load_res_table();
    const auto& paths = resources->find_resources_files();

    std::vector<std::string> styles = {"ChooseMe",
                                       "ParentWithAttr",
                                       "ChildWithParentAttr",
                                       "CustomText",
                                       "CustomText.Prickly",
                                       "CustomText.Unused",
                                       "ThemeParent",
                                       "ThemeA",
                                       "ThemeB",
                                       "ThemeUnused",
                                       "DupTheme1",
                                       "DupTheme2",
                                       "StyleNotSorted",
                                       "StyleSorted",
                                       "ThemeDifferentA",
                                       "ThemeDifferentB",
                                       "AmbiguousParent",
                                       "AmbiguousSmall",
                                       "AmbiguousBig",
                                       "SimpleParent1",
                                       "SimpleParent2",
                                       "Confusing",
                                       "Unclear",
                                       "AppTheme",
                                       "AppTheme.Light",
                                       "AppTheme.Light.Blue",
                                       "AppTheme.Light.Blue.NoActionBar"};

    uint32_t new_style_id = 0;
    for (const auto& name : styles) {
      auto ids = res_table->get_res_ids_by_name(name);
      for (const auto id : ids) {
        new_style_id = std::max(new_style_id, id);
      }
    }
    new_style_id++;
    resources::StyleModificationSpec::Modification new_style_mod(new_style_id);

    res_table->add_styles({new_style_mod}, paths);

    auto new_res_table = resources->load_res_table();
    auto style_map = new_res_table->get_style_map();

    EXPECT_THAT(style_map, Contains(Key(new_style_id)))
        << "New style with ID 0x" << std::hex << new_style_id << std::dec
        << " was not created";

    const auto& style_resources = style_map.at(new_style_id);
    EXPECT_THAT(style_resources, SizeIs(1));
    EXPECT_EQ(style_resources[0].parent, 0)
        << "New style should have no parent (0)";
    EXPECT_TRUE(style_resources[0].attributes.empty())
        << "New style should have no attributes";
  });
}
