/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "RedexResources.h"
#include "ResourceValueMergingPass.h"

ResourceAttributeInformation get_common_attributes(
    const std::vector<ResourceAttributeInformation>& attributes);

class ResourceValueMergingPassTest : public ::testing::Test {
 protected:
  ResourceValueMergingPass m_pass;

  resources::StyleResource create_style_resource(
      uint32_t parent_id, const std::vector<uint32_t>& attr_ids) {
    resources::StyleResource style;
    style.parent = parent_id;

    for (uint32_t attr_id : attr_ids) {
      resources::StyleResource::Value value(0, 0);
      style.attributes.insert({attr_id, value});
    }

    return style;
  }
};

TEST_F(ResourceValueMergingPassTest, FindCommonAttributesEmptyStyleMap) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;

  auto result = m_pass.get_resource_attributes(resource_id, style_map);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, FindCommonAttributesSingleStyle) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t attr1 = 0x7f020001;
  uint32_t attr2 = 0x7f020002;

  std::vector<resources::StyleResource> styles;
  styles.push_back(create_style_resource(0, {attr1, attr2}));
  style_map.insert({resource_id, styles});

  auto result = m_pass.get_resource_attributes(resource_id, style_map);

  EXPECT_EQ(result.size(), 2);
  EXPECT_TRUE(result.find(attr1) != result.end());
  EXPECT_TRUE(result.find(attr2) != result.end());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesEmptyVector) {
  std::vector<ResourceAttributeInformation> attributes;

  auto result = get_common_attributes(attributes);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesSingleSet) {
  std::vector<ResourceAttributeInformation> attributes;
  ResourceAttributeInformation set1;
  set1.insert(0x7f020001);
  set1.insert(0x7f020002);
  set1.insert(0x7f020003);
  attributes.push_back(set1);

  auto result = get_common_attributes(attributes);

  EXPECT_EQ(result.size(), 3);
  EXPECT_TRUE(result.find(0x7f020001) != result.end());
  EXPECT_TRUE(result.find(0x7f020002) != result.end());
  EXPECT_TRUE(result.find(0x7f020003) != result.end());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesAllCommon) {
  std::vector<ResourceAttributeInformation> attributes;

  ResourceAttributeInformation set1;
  set1.insert(0x7f020001);
  set1.insert(0x7f020002);
  attributes.push_back(set1);

  ResourceAttributeInformation set2;
  set2.insert(0x7f020001);
  set2.insert(0x7f020002);
  attributes.push_back(set2);

  ResourceAttributeInformation set3;
  set3.insert(0x7f020001);
  set3.insert(0x7f020002);
  attributes.push_back(set3);

  auto result = get_common_attributes(attributes);

  EXPECT_EQ(result.size(), 2);
  EXPECT_TRUE(result.find(0x7f020001) != result.end());
  EXPECT_TRUE(result.find(0x7f020002) != result.end());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesSomeCommon) {
  std::vector<ResourceAttributeInformation> attributes;

  ResourceAttributeInformation set1;
  set1.insert(0x7f020001);
  set1.insert(0x7f020002);
  set1.insert(0x7f020003);
  attributes.push_back(set1);

  ResourceAttributeInformation set2;
  set2.insert(0x7f020001);
  set2.insert(0x7f020004);
  attributes.push_back(set2);

  ResourceAttributeInformation set3;
  set3.insert(0x7f020001);
  set3.insert(0x7f020005);
  attributes.push_back(set3);

  auto result = get_common_attributes(attributes);

  EXPECT_EQ(result.size(), 1);
  EXPECT_TRUE(result.find(0x7f020001) != result.end());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesNoCommon) {
  std::vector<ResourceAttributeInformation> attributes;

  ResourceAttributeInformation set1;
  set1.insert(0x7f020001);
  attributes.push_back(set1);

  ResourceAttributeInformation set2;
  set2.insert(0x7f020002);
  attributes.push_back(set2);

  ResourceAttributeInformation set3;
  set3.insert(0x7f020003);
  attributes.push_back(set3);

  auto result = get_common_attributes(attributes);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesSubsetRelationship) {
  std::vector<ResourceAttributeInformation> attributes;

  ResourceAttributeInformation set1;
  set1.insert(0x7f020001);
  set1.insert(0x7f020002);
  set1.insert(0x7f020003);
  attributes.push_back(set1);

  ResourceAttributeInformation set2;
  set2.insert(0x7f020001);
  set2.insert(0x7f020002);
  attributes.push_back(set2);

  ResourceAttributeInformation set3;
  set3.insert(0x7f020001);
  attributes.push_back(set3);

  auto result = get_common_attributes(attributes);

  EXPECT_EQ(result.size(), 1);
  EXPECT_TRUE(result.find(0x7f020001) != result.end());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributesEmptySet) {
  std::vector<ResourceAttributeInformation> attributes;

  ResourceAttributeInformation set1;
  attributes.push_back(set1);

  ResourceAttributeInformation set2;
  set2.insert(0x7f020001);
  set2.insert(0x7f020002);
  attributes.push_back(set2);

  ResourceAttributeInformation set3;
  set3.insert(0x7f020001);
  attributes.push_back(set3);

  auto result = get_common_attributes(attributes);

  EXPECT_EQ(result.size(), 0);
  EXPECT_TRUE(result.find(0x7f020001) == result.end());
  EXPECT_TRUE(result.find(0x7f020002) == result.end());
  EXPECT_TRUE(result.find(0x7f020003) == result.end());
}

TEST_F(ResourceValueMergingPassTest,
       FindCommonAttributesForResourceWithNonexistentResource) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t nonexistent_resource_id = 0x7f010002;
  uint32_t attr1 = 0x7f020001;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr1, value});
  styles.push_back(style);

  style_map.insert({resource_id, styles});

  auto result =
      m_pass.get_resource_attributes(nonexistent_resource_id, style_map);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, GetCommonAttributeAmongChildrenEmptySet) {
  resources::StyleMap style_map;
  UnorderedSet<uint32_t> resource_ids;
  uint32_t attribute_id = 0x7f020001;

  auto result = m_pass.get_common_attribute_among_children(
      resource_ids, attribute_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest,
       GetCommonAttributeAmongChildrenAllSameValue) {
  resources::StyleMap style_map;
  UnorderedSet<uint32_t> resource_ids;
  uint32_t resource_id1 = 0x7f010001;
  uint32_t resource_id2 = 0x7f010002;
  uint32_t resource_id3 = 0x7f010003;
  uint32_t attribute_id = 0x7f020001;

  resources::StyleResource::Value common_value(42, 0);

  std::vector<resources::StyleResource> styles1;
  resources::StyleResource style1;
  style1.parent = 0;
  style1.attributes.insert({attribute_id, common_value});
  styles1.push_back(style1);
  style_map.insert({resource_id1, styles1});
  resource_ids.insert(resource_id1);

  std::vector<resources::StyleResource> styles2;
  resources::StyleResource style2;
  style2.parent = 0;
  style2.attributes.insert({attribute_id, common_value});
  styles2.push_back(style2);
  style_map.insert({resource_id2, styles2});
  resource_ids.insert(resource_id2);

  std::vector<resources::StyleResource> styles3;
  resources::StyleResource style3;
  style3.parent = 0;
  style3.attributes.insert({attribute_id, common_value});
  styles3.push_back(style3);
  style_map.insert({resource_id3, styles3});
  resource_ids.insert(resource_id3);

  auto result = m_pass.get_common_attribute_among_children(
      resource_ids, attribute_id, style_map);

  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(*result, common_value);
}

TEST_F(ResourceValueMergingPassTest,
       GetCommonAttributeAmongChildrenDifferentValues) {
  resources::StyleMap style_map;
  UnorderedSet<uint32_t> resource_ids;
  uint32_t resource_id1 = 0x7f010001;
  uint32_t resource_id2 = 0x7f010002;
  uint32_t attribute_id = 0x7f020001;

  std::vector<resources::StyleResource> styles1;
  resources::StyleResource style1;
  style1.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  style1.attributes.insert({attribute_id, value1});
  styles1.push_back(style1);
  style_map.insert({resource_id1, styles1});
  resource_ids.insert(resource_id1);

  std::vector<resources::StyleResource> styles2;
  resources::StyleResource style2;
  style2.parent = 0;
  resources::StyleResource::Value value2(43, 0);
  style2.attributes.insert({attribute_id, value2});
  styles2.push_back(style2);
  style_map.insert({resource_id2, styles2});
  resource_ids.insert(resource_id2);

  auto result = m_pass.get_common_attribute_among_children(
      resource_ids, attribute_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest,
       GetCommonAttributeAmongChildrenMissingAttribute) {
  resources::StyleMap style_map;
  UnorderedSet<uint32_t> resource_ids;
  uint32_t resource_id1 = 0x7f010001;
  uint32_t resource_id2 = 0x7f010002;
  uint32_t attribute_id = 0x7f020001;
  uint32_t different_attribute_id = 0x7f020002;

  std::vector<resources::StyleResource> styles1;
  resources::StyleResource style1;
  style1.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  style1.attributes.insert({attribute_id, value1});
  styles1.push_back(style1);
  style_map.insert({resource_id1, styles1});
  resource_ids.insert(resource_id1);

  std::vector<resources::StyleResource> styles2;
  resources::StyleResource style2;
  style2.parent = 0;
  resources::StyleResource::Value value2(42, 0);
  style2.attributes.insert({different_attribute_id, value2});
  styles2.push_back(style2);
  style_map.insert({resource_id2, styles2});
  resource_ids.insert(resource_id2);

  auto result = m_pass.get_common_attribute_among_children(
      resource_ids, attribute_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest,
       GetCommonAttributeAmongChildrenMultipleStyles) {
  resources::StyleMap style_map;
  UnorderedSet<uint32_t> resource_ids;
  uint32_t resource_id = 0x7f010001;
  uint32_t attribute_id = 0x7f020001;

  std::vector<resources::StyleResource> styles;

  resources::StyleResource style1;
  style1.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  style1.attributes.insert({attribute_id, value1});
  styles.push_back(style1);

  resources::StyleResource style2;
  style2.parent = 0;
  resources::StyleResource::Value value2(43, 0);
  style2.attributes.insert({attribute_id, value2});
  styles.push_back(style2);

  style_map.insert({resource_id, styles});
  resource_ids.insert(resource_id);

  auto result = m_pass.get_common_attribute_among_children(
      resource_ids, attribute_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveUnoptimizableResourcesNoneReachable) {
  OptimizableResources candidates;
  UnorderedSet<uint32_t> directly_reachable_styles;

  uint32_t resource_id1 = 0x7f010001;
  uint32_t resource_id2 = 0x7f010002;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  candidates.deletion[resource_id1].insert(attr_id1);
  candidates.deletion[resource_id2].insert(attr_id2);

  resources::StyleResource::Value value(42, 0);
  candidates.additions[resource_id1].insert({attr_id1, value});
  candidates.additions[resource_id2].insert({attr_id2, value});

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_EQ(result.deletion.size(), 2);
  EXPECT_TRUE(result.deletion.find(resource_id1) != result.deletion.end());
  EXPECT_TRUE(result.deletion.find(resource_id2) != result.deletion.end());

  EXPECT_EQ(result.additions.size(), 2);
  EXPECT_TRUE(result.additions.find(resource_id1) != result.additions.end());
  EXPECT_TRUE(result.additions.find(resource_id2) != result.additions.end());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveUnoptimizableResourcesSomeReachable) {
  OptimizableResources candidates;
  UnorderedSet<uint32_t> directly_reachable_styles;

  uint32_t resource_id1 = 0x7f010001;
  uint32_t resource_id2 = 0x7f010002;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  candidates.deletion[resource_id1].insert(attr_id1);
  candidates.deletion[resource_id2].insert(attr_id2);

  resources::StyleResource::Value value(42, 0);
  candidates.additions[resource_id1].insert({attr_id1, value});
  candidates.additions[resource_id2].insert({attr_id2, value});

  directly_reachable_styles.insert(resource_id1);

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_EQ(result.deletion.size(), 1);
  EXPECT_TRUE(result.deletion.find(resource_id1) == result.deletion.end());
  EXPECT_TRUE(result.deletion.find(resource_id2) != result.deletion.end());

  EXPECT_EQ(result.additions.size(), 1);
  EXPECT_TRUE(result.additions.find(resource_id1) == result.additions.end());
  EXPECT_TRUE(result.additions.find(resource_id2) != result.additions.end());
}

TEST_F(ResourceValueMergingPassTest, RemoveUnoptimizableResourcesAllReachable) {
  OptimizableResources candidates;
  UnorderedSet<uint32_t> directly_reachable_styles;

  uint32_t resource_id1 = 0x7f010001;
  uint32_t resource_id2 = 0x7f010002;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  candidates.deletion[resource_id1].insert(attr_id1);
  candidates.deletion[resource_id2].insert(attr_id2);

  resources::StyleResource::Value value(42, 0);
  candidates.additions[resource_id1].insert({attr_id1, value});
  candidates.additions[resource_id2].insert({attr_id2, value});

  directly_reachable_styles.insert(resource_id1);
  directly_reachable_styles.insert(resource_id2);

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_TRUE(result.deletion.empty());
  EXPECT_TRUE(result.additions.empty());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveUnoptimizableResourcesEmptyCandidates) {
  OptimizableResources candidates;
  UnorderedSet<uint32_t> directly_reachable_styles;

  directly_reachable_styles.insert(0x7f010001);
  directly_reachable_styles.insert(0x7f010002);

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_TRUE(result.deletion.empty());
  EXPECT_TRUE(result.additions.empty());
}

TEST_F(ResourceValueMergingPassTest, GetResourceAttributesEmptyStyleMap) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;

  auto result = m_pass.get_resource_attributes(resource_id, style_map);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, GetResourceAttributesNonexistentResource) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t nonexistent_resource_id = 0x7f010002;
  uint32_t attr_id = 0x7f020001;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr_id, value});
  styles.push_back(style);
  style_map.insert({resource_id, styles});

  auto result =
      m_pass.get_resource_attributes(nonexistent_resource_id, style_map);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, GetResourceAttributesSingleStyle) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr_id1, value});
  style.attributes.insert({attr_id2, value});
  styles.push_back(style);
  style_map.insert({resource_id, styles});

  auto result = m_pass.get_resource_attributes(resource_id, style_map);

  EXPECT_EQ(result.size(), 2);
  EXPECT_TRUE(result.find(attr_id1) != result.end());
  EXPECT_TRUE(result.find(attr_id2) != result.end());
}

TEST_F(ResourceValueMergingPassTest, GetResourceAttributesMultipleStyles) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  std::vector<resources::StyleResource> styles;

  resources::StyleResource style1;
  style1.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  style1.attributes.insert({attr_id1, value1});
  styles.push_back(style1);

  resources::StyleResource style2;
  style2.parent = 0;
  resources::StyleResource::Value value2(43, 0);
  style2.attributes.insert({attr_id2, value2});
  styles.push_back(style2);

  style_map.insert({resource_id, styles});

  auto result = m_pass.get_resource_attributes(resource_id, style_map);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, FindStyleResourceEmptyStyleMap) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;

  auto result = m_pass.find_style_resource(resource_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest, FindStyleResourceNonexistentResource) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t nonexistent_resource_id = 0x7f010002;
  uint32_t attr_id = 0x7f020001;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr_id, value});
  styles.push_back(style);
  style_map.insert({resource_id, styles});

  auto result = m_pass.find_style_resource(nonexistent_resource_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest, FindStyleResourceSingleStyle) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id = 0x7f020001;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr_id, value});
  styles.push_back(style);
  style_map.insert({resource_id, styles});

  auto result = m_pass.find_style_resource(resource_id, style_map);

  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->parent, 0);
  EXPECT_EQ(result->attributes.size(), 1);
  EXPECT_TRUE(result->attributes.find(attr_id) != result->attributes.end());
}

TEST_F(ResourceValueMergingPassTest, FindStyleResourceMultipleStyles) {
  resources::StyleMap style_map;
  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  std::vector<resources::StyleResource> styles;

  resources::StyleResource style1;
  style1.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  style1.attributes.insert({attr_id1, value1});
  styles.push_back(style1);

  resources::StyleResource style2;
  style2.parent = 0;
  resources::StyleResource::Value value2(43, 0);
  style2.attributes.insert({attr_id2, value2});
  styles.push_back(style2);

  style_map.insert({resource_id, styles});

  auto result = m_pass.find_style_resource(resource_id, style_map);

  EXPECT_FALSE(result.has_value());
}

TEST_F(ResourceValueMergingPassTest, ApplyAdditionsToStyleGraphEmptyAdditions) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t,
               UnorderedMap<uint32_t, resources::StyleResource::Value>>
      additions;

  m_pass.apply_additions_to_style_graph(style_info, additions);

  EXPECT_TRUE(style_info.styles.empty());
}

TEST_F(ResourceValueMergingPassTest,
       ApplyAdditionsToStyleGraphNonexistentResource) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t,
               UnorderedMap<uint32_t, resources::StyleResource::Value>>
      additions;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id = 0x7f020001;

  resources::StyleResource::Value value(42, 0);

  additions[resource_id].insert({attr_id, value});

  EXPECT_THROW(m_pass.apply_additions_to_style_graph(style_info, additions),
               RedexException);
}

TEST_F(ResourceValueMergingPassTest, ApplyAdditionsToStyleGraphSingleStyle) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t,
               UnorderedMap<uint32_t, resources::StyleResource::Value>>
      additions;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.id = resource_id;
  style.parent = 0;
  styles.push_back(style);
  style_info.styles[resource_id] = styles;

  resources::StyleResource::Value value1(42, 0);
  resources::StyleResource::Value value2(43, 0);

  additions[resource_id].insert({attr_id1, value1});
  additions[resource_id].insert({attr_id2, value2});

  m_pass.apply_additions_to_style_graph(style_info, additions);

  ASSERT_EQ(style_info.styles[resource_id].size(), 1);
  auto& updated_style = style_info.styles[resource_id][0];
  EXPECT_EQ(updated_style.attributes.size(), 2);
  EXPECT_TRUE(updated_style.attributes.find(attr_id1) !=
              updated_style.attributes.end());
  EXPECT_TRUE(updated_style.attributes.find(attr_id2) !=
              updated_style.attributes.end());

  auto attr1_it = updated_style.attributes.find(attr_id1);
  auto attr2_it = updated_style.attributes.find(attr_id2);
  EXPECT_EQ(attr1_it->second, value1);
  EXPECT_EQ(attr2_it->second, value2);
}

TEST_F(ResourceValueMergingPassTest, ApplyAdditionsToStyleGraphMultipleStyles) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t,
               UnorderedMap<uint32_t, resources::StyleResource::Value>>
      additions;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id = 0x7f020001;

  std::vector<resources::StyleResource> styles;

  resources::StyleResource style1;
  style1.id = resource_id;
  style1.parent = 0;
  styles.push_back(style1);

  resources::StyleResource style2;
  style2.id = resource_id;
  style2.parent = 0;
  styles.push_back(style2);

  style_info.styles[resource_id] = styles;

  resources::StyleResource::Value value(42, 0);

  if (additions.find(resource_id) == additions.end()) {
    additions.emplace(
        resource_id, UnorderedMap<uint32_t, resources::StyleResource::Value>());
  }
  additions[resource_id].insert({attr_id, value});

  EXPECT_THROW(m_pass.apply_additions_to_style_graph(style_info, additions),
               std::exception);
}
