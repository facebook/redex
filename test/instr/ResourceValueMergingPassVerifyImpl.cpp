/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourceValueMergingPassVerifyImpl.h"
#include "ResourceValueMergingPass.h"
#include "ResourcesTestDefs.h"
#include "verify/VerifyUtil.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
using ::testing::SizeIs;

StyleAnalysis create_style_analysis(const boost::filesystem::path& tmp_path,
                                    const Scope& classes) {
  DexStoresVector dex_stores;
  DexStore store("classes");
  store.add_classes({classes});
  dex_stores.emplace_back(store);
  resources::ReachabilityOptions options;
  ResourceConfig global_resource_config;
  return StyleAnalysis{tmp_path.string(), global_resource_config, options,
                       dex_stores, UnorderedSet<uint32_t>()};
}

const UnorderedMap<std::string, UnorderedSet<uint32_t>> INITIAL_OPTIMIZATIONS =
    {{"CardElevated", {kBackgroundAttrId}},
     {"AppTheme", {kTextColorAttrId, kBackgroundAttrId}},
     {"CardBase", {kBackgroundAttrId}}};

const UnorderedMap<std::string, UnorderedSet<uint32_t>> REMOVED_ATTRIBUTES = {
    {"AppTheme", {kTextSize, kDuplicateParentState, kStackFromBottom}},
    {"AppTheme.Light", {kTextColorAttrId, kBackgroundAttrId}},
    {"AppTheme.Light.Blue", {kColorPrimaryAttrId, kColorAccent}},
    {"BaseStyle1",
     {kTextSize, kTextColorAttrId, kDuplicateParentState, kStackFromBottom}},
    {"BaseTextStyle", {kFontFamily, kPaddingStart, kIsGame}},
    {"ButtonDanger", {kTextStyleAttrId, kTextColorAttrId}},
    {"ButtonOutline", {kTextStyleAttrId, kTextColorAttrId}},
    {"ButtonPrimary", {kTextStyleAttrId, kTextColorAttrId}},
    {"ButtonSecondary", {kTextStyleAttrId, kTextColorAttrId}},
    {"CardBase", {kFontFamily, kPaddingStart, kIsGame}},
    {"CardCompact", {kBackgroundAttrId, kBackgroundTint}},
    {"CardElevated", {kBackgroundAttrId, kBackgroundTint}},
    {"CardHighlight1", {kBackgroundAttrId}},
    {"CardHighlight2", {kBackgroundAttrId}},
    {"ChildStyle1", {kBackgroundAttrId, kDrawableStart, kDrawableEnd}},
    {"ChildStyle2", {kBackgroundAttrId, kDrawableStart, kDrawableEnd}},
    {"InputBase",
     {kTextSize, kTextColorAttrId, kDuplicateParentState, kStackFromBottom}},
    {"InputBordered", {kBackgroundAttrId}},
    {"InputRounded", {kBackgroundAttrId}},
    {"TextStyle.Body", {kTextSize}},
    {"TextStyle.Caption", {kTextSize}},
    {"TextStyle.Heading", {kTextSize}},
    {"TextStyle.Subheading", {kTextSize}},
    {"ThemeA", {kTextSize}},
    {"ThemeB", {kTextSize}},
    {"ThemeParent", {kFontFamily, kPaddingStart, kIsGame}}};

const UnorderedMap<std::string, UnorderedSet<uint32_t>> ADDED_ATTRIBUTES = {
    {"AppTheme",
     {kWindowNoTitle, kWindowActionBar, kColorPrimaryAttrId, kColorAccent}},
    {"AppTheme.Light.Blue.NoActionBar",
     {kTextColorAttrId, kBackgroundAttrId, kColorPrimaryAttrId, kColorAccent}},
    {"BaseStyle1", {kBackgroundAttrId, kDrawableStart, kDrawableEnd}},
    {"CardBase", {kBackgroundTint}},
    {"InputBase", {kBackgroundAttrId}}};

const std::vector<UnorderedSet<uint32_t>> SYNTHETIC_PARENT_ATTRIBUTE_SETS = {
    {kTextSize, kTextColorAttrId, kDuplicateParentState, kStackFromBottom},
    {kFontFamily, kPaddingStart, kIsGame},
    {kTextStyleAttrId, kTextColorAttrId}};

void verify_attribute_existance(
    ResourceTableFile* res_table,
    const UnorderedMap<std::string, UnorderedSet<uint32_t>>& attributes_map,
    bool should_exist,
    const std::string& verification_phase) {
  resources::StyleMap style_map = res_table->get_style_map();

  for (const auto& [style_name, expected_attributes] :
       UnorderedIterable(attributes_map)) {
    auto style_ids = res_table->get_res_ids_by_name(style_name);
    EXPECT_THAT(style_ids, SizeIs(1));
    uint32_t style_id = *style_ids.begin();

    EXPECT_TRUE(style_map.count(style_id) > 0)
        << "Style '" << style_name << "' not found in style_map";

    if (style_map.count(style_id) > 0) {
      const auto& style_resources = style_map.at(style_id);
      for (const auto& style_resource : style_resources) {
        for (uint32_t expected_attr : UnorderedIterable(expected_attributes)) {
          if (should_exist) {
            EXPECT_TRUE(style_resource.attributes.count(expected_attr) > 0)
                << "Attribute 0x" << std::hex << expected_attr
                << " not found in style '" << style_name << "' "
                << verification_phase;
          } else {
            EXPECT_TRUE(style_resource.attributes.count(expected_attr) == 0)
                << "Attribute 0x" << std::hex << expected_attr
                << " exists in style '" << style_name << "' "
                << verification_phase;
          }
        }
      }
    }
  }
}

void resource_value_merging_PreVerify(ResourceTableFile* res_table,
                                      StyleAnalysis* style_analysis) {
  verify_attribute_existance(res_table, REMOVED_ATTRIBUTES, true,
                             "before optimization");

  verify_attribute_existance(res_table, ADDED_ATTRIBUTES, false,
                             "before optimization");

  // Verify that these attributes are marked for deletion
  const auto& style_info = res_table->load_style_info();
  const auto& ambiguous_styles = style_analysis->ambiguous_styles();
  const auto& directly_reachable_styles =
      style_analysis->directly_reachable_styles();
  ResourceValueMergingPass pass;
  const auto& optimized_resources = pass.get_resource_optimization(
      style_info, ambiguous_styles, directly_reachable_styles);

  for (const auto& [style_name, expected_attributes] :
       UnorderedIterable(INITIAL_OPTIMIZATIONS)) {
    auto style_ids = res_table->get_res_ids_by_name(style_name);
    EXPECT_THAT(style_ids, SizeIs(1));
    uint32_t style_id = *style_ids.begin();
    UnorderedMap<uint32_t, ResourceAttributeInformation> deletion =
        optimized_resources.removals;
    EXPECT_TRUE(deletion.count(style_id) > 0)
        << "Style ID 0x" << std::hex << style_id
        << " not found in deletion map";

    if (deletion.count(style_id) > 0) {
      for (uint32_t expected_attr : UnorderedIterable(expected_attributes)) {
        EXPECT_TRUE(deletion.at(style_id).count(expected_attr) > 0)
            << "Attribute 0x" << std::hex << expected_attr
            << " not found in style '" << style_name << "'";
      }
    }
  }

  EXPECT_THAT(pass.get_config_count(*res_table), 2);
  auto app_theme_ids = res_table->get_res_ids_by_name("AppTheme");
  auto base_style_ids = res_table->get_res_ids_by_name("BaseStyle1");
  auto input_base_ids = res_table->get_res_ids_by_name("InputBase");
  EXPECT_THAT(app_theme_ids, SizeIs(1));
  EXPECT_THAT(base_style_ids, SizeIs(1));
  EXPECT_THAT(input_base_ids, SizeIs(1));
  EXPECT_THAT(pass.find_inter_graph_hoistings(style_info, ambiguous_styles),
              ::testing::UnorderedElementsAre(*app_theme_ids.begin(),
                                              *base_style_ids.begin(),
                                              *input_base_ids.begin()));
}

void verify_synthetic_parents(ResourceTableFile* res_table,
                              ResourceValueMergingPass& pass) {
  const auto& style_map = res_table->get_style_map();
  auto synthetic_styles = SYNTHETIC_PARENT_ATTRIBUTE_SETS;
  auto ids = res_table->get_res_ids_by_name(SYNTHETIC_PARENT_NAME);
  EXPECT_THAT(ids, SizeIs(synthetic_styles.size()));

  for (const auto& id : ids) {
    UnorderedSet<uint32_t> attributes =
        pass.get_resource_attributes(id, style_map);
    for (auto it = synthetic_styles.begin(); it != synthetic_styles.end();) {
      if (attributes == *it) {
        it = synthetic_styles.erase(it);
      } else {
        ++it;
      }
    }
  }
  EXPECT_THAT(synthetic_styles, ::testing::IsEmpty());
}

void resource_value_merging_PostVerify(ResourceTableFile* res_table) {
  verify_attribute_existance(res_table, REMOVED_ATTRIBUTES, false,
                             "after optimization");

  verify_attribute_existance(res_table, ADDED_ATTRIBUTES, true,
                             "after optimization");

  ResourceValueMergingPass pass;
  verify_synthetic_parents(res_table, pass);
}
