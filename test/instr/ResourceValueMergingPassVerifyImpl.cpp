/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourceValueMergingPassVerifyImpl.h"
#include "ResourceValueMergingPass.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
using ::testing::SizeIs;

constexpr uint32_t ATTR_BACKGROUND = 0x010100d4;
constexpr uint32_t ATTR_TEXT_COLOR = 0x01010098;

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

const UnorderedMap<std::string, UnorderedSet<uint32_t>> STYLE_ATTRIBUTES_MAP = {
    {"CardElevated", {ATTR_BACKGROUND}},
    {"AppTheme", {ATTR_TEXT_COLOR, ATTR_BACKGROUND}},
    {"CardBase", {ATTR_BACKGROUND}}};

void resource_value_merging_PreVerify(ResourceTableFile* res_table,
                                      StyleAnalysis* style_analysis) {
  const auto& style_info = res_table->load_style_info();
  const auto& ambiguous_styles = style_analysis->ambiguous_styles();
  const auto& directly_reachable_styles =
      style_analysis->directly_reachable_styles();
  ResourceValueMergingPass pass;
  const auto& optimized_resources = pass.get_resource_optimization(
      style_info, ambiguous_styles, directly_reachable_styles);

  for (const auto& [style_name, expected_attributes] :
       UnorderedIterable(STYLE_ATTRIBUTES_MAP)) {
    auto style_ids = res_table->get_res_ids_by_name(style_name);
    EXPECT_THAT(style_ids, SizeIs(1));
    uint32_t style_id = *style_ids.begin();
    UnorderedMap<uint32_t, ResourceAttributeInformation> deletion =
        optimized_resources.deletion;
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
}

void resource_value_merging_PostVerify(ResourceTableFile* res_table) {
  resources::StyleMap style_map = res_table->get_style_map();

  for (const auto& [style_name, expected_attributes] :
       UnorderedIterable(STYLE_ATTRIBUTES_MAP)) {
    auto style_ids = res_table->get_res_ids_by_name(style_name);

    for (uint32_t style_id : UnorderedIterable(style_ids)) {
      if (style_map.count(style_id) > 0) {

        const auto& style_resources = style_map.at(style_id);
        for (const auto& style_resource : style_resources) {
          for (uint32_t expected_attr :
               UnorderedIterable(expected_attributes)) {

            EXPECT_TRUE(style_resource.attributes.count(expected_attr) == 0)
                << "Attribute 0x" << std::hex << expected_attr
                << " not found in style '" << style_name << "'";
          }
        }

        break;
      }
    }

    EXPECT_TRUE(style_map.count(*style_ids.begin()) > 0)
        << "Style '" << style_name << "' not found in style_map";
  }
}
