/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/algorithm/string.hpp>
#include <unordered_set>

#include "ApkResources.h"
#include "RedexResources.h"
#include "utils/Serialize.h"
#include "verify/VerifyUtil.h"

namespace {

// These lists are formatted this way to make them easy to generate. Example:
// aapt d resources ~/foo.apk | grep -E "^[ ]*resource" | sed 's/: .*//' | \
//   sed 's/^[^:]*://' | sed 's/\(.*\)/"\1",/'
const std::unordered_set<std::string> KEPT_RESOURCES = {
    "array/some_fruits",
    "attr/a_boolean",
    "attr/fancy_effects",
    "attr/reverb_type",
    "attr/themeColor",
    "attr/themePadding",
    "color/bg_grey",
    "color/keep_me_unused_color",
    "color/prickly_green",
    "dimen/margin_top",
    "dimen/padding_left",
    "dimen/padding_right",
    "dimen/welcome_text_size",
    "drawable/icon",
    "drawable/prickly",
    "id/delay",
    "id/distortion",
    "id/hall",
    "id/overdrive",
    "id/plate",
    "id/reverb",
    "id/shimmer",
    "id/spring",
    "id/welcome_view",
    "layout/activity_main",
    "layout/themed",
    "plurals/a_sentence_with_geese",
    "string/app_name",
    "string/button_txt",
    "string/keep_me_unused_str",
    "string/log_msg",
    "string/toast_fmt",
    "string/too_many",
    "string/used_from_layout",
    "string/welcome",
    "string/yummy_orange",
    "style/CustomText",
    "style/CustomText.Prickly",
    "style/ThemeA",
    "style/ThemeB",
};

// <declare-styleable> value names will generate entries in resource table, but
// not R fields, so don't run ID comparisons on these.
const std::unordered_set<std::string> NO_FIELD_RESOURCES = {
    "id/delay", "id/distortion", "id/hall",    "id/overdrive",
    "id/plate", "id/reverb",     "id/shimmer", "id/spring",
};

const std::unordered_set<std::string> ADDITIONAL_KEPT_RESOURCES = {
    "dimen/bar",
    "dimen/small",
    "dimen/medium2",
    "dimen/medium",
    "string/_an_unused_string",
    "attr/SameAttributeA",
    "color/hex_or_file2",
};

const std::unordered_set<std::string> UNUSED_RESOURCES = {
    "array/unused_fruits",
    "attr/SameAttributeA",
    "attr/SameAttributeB",
    "attr/themeUnused",
    "color/hex_or_file",
    "color/hex_or_file2",
    "dimen/bar",
    "dimen/baz",
    "dimen/boo",
    "dimen/far",
    "dimen/foo",
    "dimen/medium",
    "dimen/medium2",
    "dimen/small",
    "dimen/unused_dimen_1",
    "dimen/unused_dimen_2",
    "dimen/foo",
    "drawable/x_icon",
    "drawable/x_prickly",
    "string/_an_unused_string",
    "string/unused_durian",
    "string/unused_pineapple",
    "string/unused_str",
    "style/CustomText.Unused",
    "style/ThemeDifferentA",
    "style/ThemeDifferentB",
    "style/ThemeUnused",
};

const std::unordered_set<std::string> KEPT_FILE_PATHS = {
    "res/drawable-mdpi-v4/icon.png",
    "res/drawable-mdpi-v4/prickly.png",
    "res/layout/activity_main.xml",
    "res/layout/themed.xml",
};

const std::unordered_set<std::string> REMOVED_FILE_PATHS = {
    "res/color/hex_or_file2.xml",
    "res/color-night-v8/hex_or_file.xml",
    "res/drawable-mdpi-v4/x_icon.png",
    "res/drawable-mdpi-v4/x_prickly.png",
};

const std::unordered_set<std::string> ADDITIONAL_KEPT_FILE_PATHS = {
    "res/color/hex_or_file2.xml",
};

std::unordered_set<std::string> get_resource_names_of_type(
    const std::unordered_set<std::string>& list, const std::string& type) {
  std::unordered_set<std::string> result;
  for (const auto& str : list) {
    if (str.find(type) == 0) {
      result.emplace(str.substr(str.find('/') + 1));
    }
  }
  return result;
}

// Asserts that for a given resource type (dimen, string, drawable, etc) that
// all resource names have a corresponding value and that the range of values is
// contiguous from [0, list.size())
void assert_type_contiguous(const std::unordered_set<std::string>& list,
                            const std::string& type,
                            ResourceTableFile* res_table) {
  auto resources = get_resource_names_of_type(list, type);
  std::unordered_set<uint32_t> values;
  for (const auto& resource : resources) {
    auto ids = res_table->get_res_ids_by_name(resource);
    EXPECT_EQ(ids.size(), 1) << "Expected only 1 resource ID for " << resource;
    // Don't care about package ID or type ID, just get the entry IDs.
    values.emplace(ids[0] & 0xFFFF);
  }
  EXPECT_EQ(resources.size(), values.size()) << "Resource values not unique";
  for (uint32_t i = 0; i < resources.size(); i++) {
    EXPECT_EQ(values.count(i), 1) << "Values are not contiguous, missing " << i;
  }
}

// Asserts that for a given resource type (dimen, string, drawable, etc) that
// all resources under used_list are kept and not nullified, resources not
// under used_list still have entry but nullified, and resources after
// current_entry_num are removed.
void assert_type_nullified(const std::unordered_set<std::string>& used_list,
                           const std::string& type,
                           int original_entry_num,
                           int current_entry_num,
                           ResourceTableFile* res_table) {
  auto used_resources = get_resource_names_of_type(used_list, type);
  std::unordered_set<uint32_t> values;
  uint32_t package_and_type = 0;
  for (const auto& resource : used_resources) {
    auto ids = res_table->get_res_ids_by_name(resource);
    EXPECT_EQ(ids.size(), 1) << "Expected only 1 resource ID for " << resource;
    // Don't care about package ID or type ID, just get the entry IDs.
    values.emplace(ids[0] & 0xFFFF);
    package_and_type = ids[0] & 0xFFFF0000;
  }
  EXPECT_NE(package_and_type, 0)
      << "package_and_type remains zero after going through kepted list: "
      << type;
  for (uint32_t i = 0; i < original_entry_num; i++) {
    auto res_id = package_and_type | i;
    if (i >= current_entry_num) {
      EXPECT_EQ(res_table->id_to_name.count(res_id), 0)
          << "Values after current all entries still exist: " << res_id;
    } else if (values.count(i) == 0) {
      EXPECT_EQ(res_table->resource_value_count(res_id), 0)
          << "Values are not nullified: " << res_id;
    } else {
      EXPECT_NE(res_table->resource_value_count(res_id), 0)
          << "Values are nullified: " << res_id;
    }
  }
}

// Asserts that all given resources in the vector have the given number of IDs
// in the resource table, if nonzero that the corresponding R class has a static
// field with the same value.
void run_restable_field_validation(
    const DexClasses& classes,
    const std::unordered_set<std::string>& values_to_check,
    const int num_expected_ids,
    ResourceTableFile* res_table) {
  for (const auto& resource : values_to_check) {
    auto idx = resource.find('/');
    auto type = resource.substr(0, idx);
    auto name = resource.substr(idx + 1);

    auto ids = res_table->get_res_ids_by_name(name);
    EXPECT_EQ(ids.size(), num_expected_ids)
        << "Incorrect number of IDs for " << resource;

    if (num_expected_ids == 0) {
      // No more validation to do
      continue;
    }
    if (NO_FIELD_RESOURCES.count(resource) > 0) {
      // Don't look for a field if the ID is known to not generate fields.
      continue;
    }

    auto r_cls_name = "Lcom/facebook/R$" + type + ";";
    auto r_cls = find_class_named(classes, r_cls_name.c_str());
    boost::replace_all(name, ".", "_");
    auto field = find_sfield_named(*r_cls, name.c_str());
    EXPECT_NE(nullptr, field)
        << "Could not find static R field for " << resource;
    EXPECT_EQ(ids[0], field->get_static_value()->value())
        << "Constant value mismatch between resource table and R class for "
        << resource;
  }
}

} // namespace

void preverify_impl(const DexClasses& classes, ResourceTableFile* res_table) {
  run_restable_field_validation(classes, KEPT_RESOURCES, 1, res_table);
  run_restable_field_validation(classes, UNUSED_RESOURCES, 1, res_table);
}

void postverify_impl(const DexClasses& classes, ResourceTableFile* res_table) {
  run_restable_field_validation(classes, KEPT_RESOURCES, 1, res_table);
  run_restable_field_validation(classes, UNUSED_RESOURCES, 0, res_table);
  // Spot check a couple of types that had several things deleted, to make sure
  // ID range is sensible.
  assert_type_contiguous(KEPT_RESOURCES, "string", res_table);
  assert_type_contiguous(KEPT_RESOURCES, "dimen", res_table);
}

void preverify_nullify_impl(const DexClasses& classes,
                            ResourceTableFile* res_table) {
  run_restable_field_validation(classes, KEPT_RESOURCES, 1, res_table);
  run_restable_field_validation(classes, UNUSED_RESOURCES, 1, res_table);
}

void postverify_nullify_impl(const DexClasses& classes,
                             ResourceTableFile* res_table) {
  auto modified_kept_resources = KEPT_RESOURCES;
  auto modified_unused_resources = UNUSED_RESOURCES;
  // Firstly make sure the resource name and resource id pair is as expected
  auto ids = res_table->get_res_ids_by_name("bar");
  EXPECT_EQ(ids.size(), 1);
  EXPECT_EQ(ids[0], 0x7f040000);
  ids = res_table->get_res_ids_by_name("_an_unused_string");
  EXPECT_EQ(ids.size(), 1);
  EXPECT_EQ(ids[0], 0x7f090000);
  ids = res_table->get_res_ids_by_name("hex_or_file2");
  EXPECT_EQ(ids.size(), 1);
  EXPECT_EQ(ids[0], 0x7f030003);
  ids = res_table->get_res_ids_by_name("SameAttributeA");
  EXPECT_EQ(ids.size(), 1);
  EXPECT_EQ(ids[0], 0x7f020000);
  ids = res_table->get_res_ids_by_name("medium2");
  EXPECT_EQ(ids.size(), 1);
  EXPECT_EQ(ids[0], 0x7f040008);
  for (const auto& resource_name : ADDITIONAL_KEPT_RESOURCES) {
    modified_unused_resources.erase(resource_name);
    modified_kept_resources.emplace(resource_name);
  }
  run_restable_field_validation(classes, modified_kept_resources, 1, res_table);
  run_restable_field_validation(
      classes, modified_unused_resources, 0, res_table);
  // Spot check a couple of types that had several things deleted, to make sure
  // ID range is sensible.
  assert_type_nullified(modified_kept_resources, "string", 14, 14, res_table);
  assert_type_nullified(modified_kept_resources, "dimen", 15, 15, res_table);
  assert_type_nullified(modified_kept_resources, "array", 2, 1, res_table);
  assert_type_nullified(modified_kept_resources, "style", 12, 9, res_table);
  assert_type_nullified(modified_kept_resources, "drawable", 4, 2, res_table);
}

void apk_postverify_impl(ResourcesArscFile* res_table) {
  auto& pool = res_table->get_table_snapshot().get_global_strings();
  std::unordered_set<std::string> global_strings;
  for (int i = 0; i < pool.size(); i++) {
    global_strings.emplace(arsc::get_string_from_pool(pool, i));
  }
  for (const auto& s : KEPT_FILE_PATHS) {
    EXPECT_EQ(global_strings.count(s), 1)
        << "Global string pool should contain string " << s.c_str();
  }
  for (const auto& s : REMOVED_FILE_PATHS) {
    EXPECT_EQ(global_strings.count(s), 0)
        << "Global string pool should NOT contain string " << s.c_str();
  }
}

void apk_postverify_nullify_impl(ResourcesArscFile* res_table) {
  auto modified_kept_file_paths = KEPT_FILE_PATHS;
  auto modified_removed_file_paths = REMOVED_FILE_PATHS;
  for (const auto& resource_name : ADDITIONAL_KEPT_FILE_PATHS) {
    modified_removed_file_paths.erase(resource_name);
    modified_kept_file_paths.emplace(resource_name);
  }
  auto& pool = res_table->get_table_snapshot().get_global_strings();
  std::unordered_set<std::string> global_strings;
  for (int i = 0; i < pool.size(); i++) {
    global_strings.emplace(arsc::get_string_from_pool(pool, i));
  }
  for (const auto& s : modified_kept_file_paths) {
    EXPECT_EQ(global_strings.count(s), 1)
        << "Global string pool should contain string " << s.c_str();
  }
  for (const auto& s : modified_removed_file_paths) {
    EXPECT_EQ(global_strings.count(s), 0)
        << "Global string pool should NOT contain string " << s.c_str();
  }
}
