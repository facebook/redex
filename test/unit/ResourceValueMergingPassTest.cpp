/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ResourceValueMergingPass.h"

ResourceAttributeInformation get_common_attributes(
    const std::vector<ResourceAttributeInformation>& attributes);

class ResourceValueMergingPassTest : public ::testing::Test {
 protected:
  ResourceValueMergingPass m_pass;
  resources::StyleInfo::vertex_t add_vertex(resources::StyleInfo& style_info,
                                            uint32_t id) {
    auto vertex =
        boost::add_vertex(resources::StyleInfo::Node{id}, style_info.graph);
    style_info.id_to_vertex[id] = vertex;
    return vertex;
  }

  void add_edge(resources::StyleInfo& style_info,
                resources::StyleInfo::vertex_t parent,
                resources::StyleInfo::vertex_t child) {
    boost::add_edge(parent, child, style_info.graph);
  }
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

  candidates.removals[resource_id1].insert(attr_id1);
  candidates.removals[resource_id2].insert(attr_id2);

  resources::StyleResource::Value value(42, 0);
  candidates.additions[resource_id1].insert({attr_id1, value});
  candidates.additions[resource_id2].insert({attr_id2, value});

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_EQ(result.removals.size(), 2);
  EXPECT_TRUE(result.removals.find(resource_id1) != result.removals.end());
  EXPECT_TRUE(result.removals.find(resource_id2) != result.removals.end());

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

  candidates.removals[resource_id1].insert(attr_id1);
  candidates.removals[resource_id2].insert(attr_id2);

  resources::StyleResource::Value value(42, 0);
  candidates.additions[resource_id1].insert({attr_id1, value});
  candidates.additions[resource_id2].insert({attr_id2, value});

  directly_reachable_styles.insert(resource_id1);

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_EQ(result.removals.size(), 1);
  EXPECT_TRUE(result.removals.find(resource_id1) == result.removals.end());
  EXPECT_TRUE(result.removals.find(resource_id2) != result.removals.end());

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

  candidates.removals[resource_id1].insert(attr_id1);
  candidates.removals[resource_id2].insert(attr_id2);

  resources::StyleResource::Value value(42, 0);
  candidates.additions[resource_id1].insert({attr_id1, value});
  candidates.additions[resource_id2].insert({attr_id2, value});

  directly_reachable_styles.insert(resource_id1);
  directly_reachable_styles.insert(resource_id2);

  auto result = m_pass.remove_unoptimizable_resources(
      candidates, directly_reachable_styles);

  EXPECT_TRUE(result.removals.empty());
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

  EXPECT_TRUE(result.removals.empty());
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

TEST_F(ResourceValueMergingPassTest, ApplyRemovalsToStyleGraphEmptyRemovals) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  m_pass.apply_removals_to_style_graph(style_info, removals);

  EXPECT_TRUE(style_info.styles.empty());
}

TEST_F(ResourceValueMergingPassTest,
       ApplyRemovalsToStyleGraphNonexistentResource) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id = 0x7f020001;

  removals[resource_id].insert(attr_id);

  EXPECT_THROW(m_pass.apply_removals_to_style_graph(style_info, removals),
               RedexException);
}

TEST_F(ResourceValueMergingPassTest, ApplyRemovalsToStyleGraphSingleStyle) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;
  uint32_t attr_id3 = 0x7f020003;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.id = resource_id;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr_id1, value});
  style.attributes.insert({attr_id2, value});
  style.attributes.insert({attr_id3, value});
  styles.push_back(style);
  style_info.styles[resource_id] = styles;

  removals[resource_id].insert(attr_id1);
  removals[resource_id].insert(attr_id2);

  m_pass.apply_removals_to_style_graph(style_info, removals);

  ASSERT_EQ(style_info.styles[resource_id].size(), 1);
  auto& updated_style = style_info.styles[resource_id][0];
  EXPECT_EQ(updated_style.attributes.size(), 1);
  EXPECT_TRUE(updated_style.attributes.find(attr_id1) ==
              updated_style.attributes.end());
  EXPECT_TRUE(updated_style.attributes.find(attr_id2) ==
              updated_style.attributes.end());
  EXPECT_TRUE(updated_style.attributes.find(attr_id3) !=
              updated_style.attributes.end());
}

TEST_F(ResourceValueMergingPassTest,
       ApplyRemovalsToStyleGraphNonexistentAttribute) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;
  uint32_t nonexistent_attr_id = 0x7f020003;

  std::vector<resources::StyleResource> styles;
  resources::StyleResource style;
  style.id = resource_id;
  style.parent = 0;
  resources::StyleResource::Value value(42, 0);
  style.attributes.insert({attr_id1, value});
  style.attributes.insert({attr_id2, value});
  styles.push_back(style);
  style_info.styles[resource_id] = styles;

  removals[resource_id].insert(attr_id1);
  removals[resource_id].insert(nonexistent_attr_id);

  EXPECT_THROW(m_pass.apply_removals_to_style_graph(style_info, removals),
               RedexException);
}

TEST_F(ResourceValueMergingPassTest, ApplyRemovalsToStyleGraphMultipleStyles) {
  resources::StyleInfo style_info;
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

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

  removals[resource_id].insert(attr_id);

  EXPECT_THROW(m_pass.apply_removals_to_style_graph(style_info, removals),
               std::exception);
}

resources::StyleInfo setup_test_style_info(bool include_grandchildren = false) {
  resources::StyleInfo style_info;

  auto root = boost::add_vertex(style_info.graph);
  auto child1 = boost::add_vertex(style_info.graph);

  style_info.graph[root].id = 0x7f010001;
  style_info.graph[child1].id = 0x7f010002;

  boost::add_edge(root, child1, style_info.graph);

  resources::StyleResource::Value value1(42, 0);
  resources::StyleResource::Value value2(43, 0);

  resources::StyleResource root_style;
  root_style.id = 0x7f010001;
  root_style.parent = 0;
  root_style.attributes.insert({0x7f020001, value1});
  root_style.attributes.insert({0x7f020002, value2});
  style_info.styles[0x7f010001].push_back(root_style);

  resources::StyleResource child1_style;
  child1_style.id = 0x7f010002;
  child1_style.parent = 0x7f010001;
  child1_style.attributes.insert({0x7f020001, value1});
  child1_style.attributes.insert({0x7f020003, value2});
  style_info.styles[0x7f010002].push_back(child1_style);

  if (include_grandchildren) {
    auto child2 = boost::add_vertex(style_info.graph);
    auto grandchild1 = boost::add_vertex(style_info.graph);
    auto grandchild2 = boost::add_vertex(style_info.graph);

    style_info.graph[child2].id = 0x7f010003;
    style_info.graph[grandchild1].id = 0x7f010004;
    style_info.graph[grandchild2].id = 0x7f010005;

    boost::add_edge(root, child2, style_info.graph);
    boost::add_edge(child1, grandchild1, style_info.graph);
    boost::add_edge(child1, grandchild2, style_info.graph);

    resources::StyleResource child2_style;
    child2_style.id = 0x7f010003;
    child2_style.parent = 0x7f010001;
    child2_style.attributes.insert({0x7f020001, value1});
    child2_style.attributes.insert({0x7f020004, value2});
    style_info.styles[0x7f010003].push_back(child2_style);

    resources::StyleResource grandchild1_style;
    grandchild1_style.id = 0x7f010004;
    grandchild1_style.parent = 0x7f010002;
    grandchild1_style.attributes.insert({0x7f020001, value1});
    grandchild1_style.attributes.insert({0x7f020005, value2});
    style_info.styles[0x7f010004].push_back(grandchild1_style);

    resources::StyleResource grandchild2_style;
    grandchild2_style.id = 0x7f010005;
    grandchild2_style.parent = 0x7f010002;
    grandchild2_style.attributes.insert({0x7f020001, value1});
    grandchild2_style.attributes.insert({0x7f020006, value2});
    style_info.styles[0x7f010005].push_back(grandchild2_style);
  }

  return style_info;
}

TEST_F(ResourceValueMergingPassTest, GetGraphDiffsSimple) {
  resources::StyleInfo initial_style_info = setup_test_style_info(true);
  resources::StyleInfo optimized_style_info = initial_style_info;

  // 1. Remove attribute 0x7f020001 from root (0x7f010001) as it's common in all
  // children
  m_pass.apply_removals_to_style_graph(optimized_style_info,
                                       {{0x7f010001, {0x7f020001}}});

  // 2. Add attribute 0x7f020007 to child1 (0x7f010002) as it's common in all
  // its children
  resources::StyleResource::Value value3(44, 0);
  optimized_style_info.styles[0x7f010002][0].attributes.insert(
      {0x7f020007, value3});

  auto diffs = m_pass.get_graph_diffs(initial_style_info, optimized_style_info,
                                      UnorderedSet<uint32_t>());

  // Removals
  EXPECT_EQ(diffs.removals.size(), 1);
  EXPECT_TRUE(diffs.removals.find(0x7f010001) != diffs.removals.end());
  EXPECT_EQ(diffs.removals[0x7f010001].size(), 1);
  EXPECT_TRUE(diffs.removals[0x7f010001].find(0x7f020001) !=
              diffs.removals[0x7f010001].end());

  // Additions
  EXPECT_EQ(diffs.additions.size(), 1);
  EXPECT_TRUE(diffs.additions.find(0x7f010002) != diffs.additions.end());
  EXPECT_EQ(diffs.additions[0x7f010002].size(), 1);
  EXPECT_TRUE(diffs.additions[0x7f010002].find(0x7f020007) !=
              diffs.additions[0x7f010002].end());
}

TEST_F(ResourceValueMergingPassTest, GetGraphDiffsWithAmbiguousStyles) {
  resources::StyleInfo initial_style_info = setup_test_style_info();
  resources::StyleInfo optimized_style_info = initial_style_info;

  // 1. Remove attribute 0x7f020001 from root (0x7f010001)
  m_pass.apply_removals_to_style_graph(optimized_style_info,
                                       {{0x7f010001, {0x7f020001}}});

  // 2. Add attribute 0x7f020007 to child1 (0x7f010002)
  resources::StyleResource::Value value3(44, 0);
  optimized_style_info.styles[0x7f010002][0].attributes.insert(
      {0x7f020007, value3});

  // Mark root as ambiguous
  UnorderedSet<uint32_t> ambiguous_styles = {0x7f010001};

  auto diffs = m_pass.get_graph_diffs(initial_style_info, optimized_style_info,
                                      ambiguous_styles);

  // Root is ambiguous, so no removals should be applied to it
  EXPECT_TRUE(diffs.removals.find(0x7f010001) == diffs.removals.end());

  // Additions to child1 should still be present
  EXPECT_EQ(diffs.additions.size(), 1);
  EXPECT_TRUE(diffs.additions.find(0x7f010002) != diffs.additions.end());
  EXPECT_EQ(diffs.additions[0x7f010002].size(), 1);
  EXPECT_TRUE(diffs.additions[0x7f010002].find(0x7f020007) !=
              diffs.additions[0x7f010002].end());
}

TEST_F(ResourceValueMergingPassTest, GetGraphDiffsWithModifiedAttributeValues) {
  resources::StyleInfo initial_style_info = setup_test_style_info();
  resources::StyleInfo optimized_style_info = initial_style_info;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id = 0x7f020001;
  resources::StyleResource::Value new_value(99, 0);

  optimized_style_info.styles[resource_id][0].attributes.insert_or_assign(
      attr_id, new_value);

  UnorderedSet<uint32_t> ambiguous_styles;

  auto diffs = m_pass.get_graph_diffs(initial_style_info, optimized_style_info,
                                      ambiguous_styles);

  // The modified attribute should appear in both removals and additions
  EXPECT_EQ(diffs.removals.size(), 1);
  EXPECT_TRUE(diffs.removals.find(resource_id) != diffs.removals.end());
  EXPECT_EQ(diffs.removals[resource_id].size(), 1);
  EXPECT_TRUE(diffs.removals[resource_id].find(attr_id) !=
              diffs.removals[resource_id].end());

  EXPECT_EQ(diffs.additions.size(), 1);
  EXPECT_TRUE(diffs.additions.find(resource_id) != diffs.additions.end());
  EXPECT_EQ(diffs.additions[resource_id].size(), 1);
  EXPECT_TRUE(diffs.additions[resource_id].find(attr_id) !=
              diffs.additions[resource_id].end());

  // Verify the new value is correctly stored in additions
  auto addition_it = diffs.additions[resource_id].find(attr_id);
  EXPECT_TRUE(addition_it != diffs.additions[resource_id].end());
  EXPECT_EQ(addition_it->second, new_value);
}

TEST_F(ResourceValueMergingPassTest,
       GetGraphDiffsWithMultipleModifiedAttributes) {
  resources::StyleInfo initial_style_info = setup_test_style_info();
  resources::StyleInfo optimized_style_info = initial_style_info;

  uint32_t resource_id = 0x7f010001;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  resources::StyleResource::Value new_value1(100, 0);
  resources::StyleResource::Value new_value2(200, 0);

  optimized_style_info.styles[resource_id][0].attributes.insert_or_assign(
      attr_id1, new_value1);
  optimized_style_info.styles[resource_id][0].attributes.insert_or_assign(
      attr_id2, new_value2);

  UnorderedSet<uint32_t> ambiguous_styles;

  auto diffs = m_pass.get_graph_diffs(initial_style_info, optimized_style_info,
                                      ambiguous_styles);

  // Both modified attributes should appear in removals and additions
  EXPECT_EQ(diffs.removals.size(), 1);
  EXPECT_TRUE(diffs.removals.find(resource_id) != diffs.removals.end());
  EXPECT_EQ(diffs.removals[resource_id].size(), 2);
  EXPECT_TRUE(diffs.removals[resource_id].find(attr_id1) !=
              diffs.removals[resource_id].end());
  EXPECT_TRUE(diffs.removals[resource_id].find(attr_id2) !=
              diffs.removals[resource_id].end());

  EXPECT_EQ(diffs.additions.size(), 1);
  EXPECT_TRUE(diffs.additions.find(resource_id) != diffs.additions.end());
  EXPECT_EQ(diffs.additions[resource_id].size(), 2);
  EXPECT_TRUE(diffs.additions[resource_id].find(attr_id1) !=
              diffs.additions[resource_id].end());
  EXPECT_TRUE(diffs.additions[resource_id].find(attr_id2) !=
              diffs.additions[resource_id].end());

  // Verify the new values are correctly stored
  auto addition1_it = diffs.additions[resource_id].find(attr_id1);
  auto addition2_it = diffs.additions[resource_id].find(attr_id2);
  EXPECT_TRUE(addition1_it != diffs.additions[resource_id].end());
  EXPECT_TRUE(addition2_it != diffs.additions[resource_id].end());
  EXPECT_EQ(addition1_it->second, new_value1);
  EXPECT_EQ(addition2_it->second, new_value2);
}

TEST_F(ResourceValueMergingPassTest, RemoveAttributeFromDescendentSingleChild) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, parent_vertex, child_vertex);

  uint32_t attr_id = 0x01010001;
  resources::StyleResource::Value attr_value(1, 0x12345678);
  UnorderedMap<uint32_t, resources::StyleResource::Value> attr_map;
  attr_map.insert({attr_id, attr_value});

  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  m_pass.remove_attribute_from_descendent(parent_id, attr_map, style_info,
                                          removals);

  ASSERT_EQ(removals.size(), 1);
  ASSERT_TRUE(removals.find(child_id) != removals.end());
  ASSERT_EQ(removals[child_id].size(), 1);
  ASSERT_TRUE(removals[child_id].find(attr_id) != removals[child_id].end());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveAttributeFromDescendentMultipleChildren) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child1_id = 0x7f010002;
  uint32_t child2_id = 0x7f010003;
  uint32_t child3_id = 0x7f010004;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child1_vertex = add_vertex(style_info, child1_id);
  auto child2_vertex = add_vertex(style_info, child2_id);
  auto child3_vertex = add_vertex(style_info, child3_id);

  add_edge(style_info, parent_vertex, child1_vertex);
  add_edge(style_info, parent_vertex, child2_vertex);
  add_edge(style_info, parent_vertex, child3_vertex);

  uint32_t attr_id = 0x01010001;
  resources::StyleResource::Value attr_value(1, 0x12345678);
  UnorderedMap<uint32_t, resources::StyleResource::Value> attr_map;
  attr_map.insert({attr_id, attr_value});

  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  m_pass.remove_attribute_from_descendent(parent_id, attr_map, style_info,
                                          removals);

  ASSERT_EQ(removals.size(), 3);
  ASSERT_TRUE(removals.find(child1_id) != removals.end());
  ASSERT_TRUE(removals.find(child2_id) != removals.end());
  ASSERT_TRUE(removals.find(child3_id) != removals.end());

  ASSERT_EQ(removals[child1_id].size(), 1);
  ASSERT_TRUE(removals[child1_id].find(attr_id) != removals[child1_id].end());

  ASSERT_EQ(removals[child2_id].size(), 1);
  ASSERT_TRUE(removals[child2_id].find(attr_id) != removals[child2_id].end());

  ASSERT_EQ(removals[child3_id].size(), 1);
  ASSERT_TRUE(removals[child3_id].find(attr_id) != removals[child3_id].end());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveAttributeFromDescendentMultipleAttributes) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, parent_vertex, child_vertex);

  uint32_t attr_id1 = 0x01010001;
  uint32_t attr_id2 = 0x01010002;
  uint32_t attr_id3 = 0x01010003;

  resources::StyleResource::Value attr_value1(1, 0x12345678);
  resources::StyleResource::Value attr_value2(2, std::string("test_value"));
  resources::StyleResource::Value attr_value3(1, 0x87654321);

  UnorderedMap<uint32_t, resources::StyleResource::Value> attr_map;
  attr_map.insert({attr_id1, attr_value1});
  attr_map.insert({attr_id2, attr_value2});
  attr_map.insert({attr_id3, attr_value3});

  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  m_pass.remove_attribute_from_descendent(parent_id, attr_map, style_info,
                                          removals);

  ASSERT_EQ(removals.size(), 1);
  ASSERT_TRUE(removals.find(child_id) != removals.end());

  ASSERT_EQ(removals[child_id].size(), 3);
  ASSERT_TRUE(removals[child_id].find(attr_id1) != removals[child_id].end());
  ASSERT_TRUE(removals[child_id].find(attr_id2) != removals[child_id].end());
  ASSERT_TRUE(removals[child_id].find(attr_id3) != removals[child_id].end());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveAttributeFromDescendentMultiLevelHierarchy) {
  resources::StyleInfo style_info;

  uint32_t grandparent_id = 0x7f010001;
  uint32_t parent_id = 0x7f010002;
  uint32_t child_id = 0x7f010003;

  auto grandparent_vertex = add_vertex(style_info, grandparent_id);
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, grandparent_vertex, parent_vertex);
  add_edge(style_info, parent_vertex, child_vertex);

  uint32_t attr_id = 0x01010001;
  resources::StyleResource::Value attr_value(1, 0x12345678);
  UnorderedMap<uint32_t, resources::StyleResource::Value> attr_map;
  attr_map.insert({attr_id, attr_value});

  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  m_pass.remove_attribute_from_descendent(parent_id, attr_map, style_info,
                                          removals);

  ASSERT_EQ(removals.size(), 1);
  ASSERT_TRUE(removals.find(child_id) != removals.end());
  ASSERT_TRUE(removals.find(parent_id) == removals.end());

  ASSERT_EQ(removals[child_id].size(), 1);
  ASSERT_TRUE(removals[child_id].find(attr_id) != removals[child_id].end());
}

TEST_F(ResourceValueMergingPassTest,
       RemoveAttributeFromDescendentEmptyAttributeMap) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedMap<uint32_t, resources::StyleResource::Value> attr_map;
  UnorderedMap<uint32_t, ResourceAttributeInformation> removals;

  m_pass.remove_attribute_from_descendent(parent_id, attr_map, style_info,
                                          removals);

  ASSERT_EQ(removals.size(), 0);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeSimpleParentChild) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0][0], parent_id);
}

TEST_F(ResourceValueMergingPassTest,
       GetResourcesToMergeParentWithMultipleChildren) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child1_id = 0x7f010002;
  uint32_t child2_id = 0x7f010003;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child1_vertex = add_vertex(style_info, child1_id);
  auto child2_vertex = add_vertex(style_info, child2_id);

  add_edge(style_info, parent_vertex, child1_vertex);
  add_edge(style_info, parent_vertex, child2_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Parent has multiple children, so no merging should occur
  EXPECT_EQ(result.size(), 0);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeWithAmbiguousParent) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles = {parent_id};
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Parent is ambiguous, so no merging should occur
  EXPECT_EQ(result.size(), 0);
}

TEST_F(ResourceValueMergingPassTest,
       GetResourcesToMergeWithDirectlyReachableParent) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles = {parent_id};

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Parent is directly reachable, so no merging should occur
  EXPECT_EQ(result.size(), 0);
}

TEST_F(ResourceValueMergingPassTest,
       GetResourcesToMergeWithDirectlyReachableChild) {
  resources::StyleInfo style_info;

  uint32_t parent_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles = {child_id};

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Merging can occur even if a child is directly reachable
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0][0], parent_id);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeChainedMergesPreOrder) {
  resources::StyleInfo style_info;

  // Create a chain: grandparent -> parent -> child
  // where both grandparent and parent have exactly one child
  uint32_t grandparent_id = 0x7f010001;
  uint32_t parent_id = 0x7f010002;
  uint32_t child_id = 0x7f010003;

  auto grandparent_vertex = add_vertex(style_info, grandparent_id);
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, grandparent_vertex, parent_vertex);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Should have two merge pairs: (grandparent, parent) and (parent, child)
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].size(), 2);

  // Verify pre-order traversal: grandparent->parent should come before
  // parent->child
  EXPECT_THAT(result[0], ::testing::ElementsAre(grandparent_id, parent_id));
}

TEST_F(ResourceValueMergingPassTest,
       GetResourcesToMergeChainedMergesWithDirectlyReachableParent) {
  resources::StyleInfo style_info;

  // Create a chain: grandparent -> parent -> child
  // where both grandparent and parent have exactly one child
  uint32_t grandparent_id = 0x7f010001;
  uint32_t parent_id = 0x7f010002;
  uint32_t child_id = 0x7f010003;

  auto grandparent_vertex = add_vertex(style_info, grandparent_id);
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, grandparent_vertex, parent_vertex);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles = {parent_id};

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Should have one merge value: grandparent
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0][0], grandparent_id);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeComplexHierarchy) {
  resources::StyleInfo style_info;

  // Create a more complex hierarchy:
  //     root
  //    /    \
  //   A      B (B has 2 children, so no merge)
  //   |     / \
  //   C    D   E
  //   |
  //   F

  uint32_t root_id = 0x7f010001;
  uint32_t a_id = 0x7f010002;
  uint32_t b_id = 0x7f010003;
  uint32_t c_id = 0x7f010004;
  uint32_t d_id = 0x7f010005;
  uint32_t e_id = 0x7f010006;
  uint32_t f_id = 0x7f010007;

  auto root_vertex = add_vertex(style_info, root_id);
  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);
  auto f_vertex = add_vertex(style_info, f_id);

  add_edge(style_info, root_vertex, a_vertex);
  add_edge(style_info, root_vertex, b_vertex);
  add_edge(style_info, a_vertex, c_vertex);
  add_edge(style_info, b_vertex, d_vertex);
  add_edge(style_info, b_vertex, e_vertex);
  add_edge(style_info, c_vertex, f_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Expected merges: (A, C) and (C, F)
  // Root has 2 children, so no merge
  // B has 2 children, so no merge
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].size(), 2);

  EXPECT_THAT(result[0], ::testing::ElementsAre(a_id, c_id));
}

TEST_F(ResourceValueMergingPassTest,
       GetResourcesToMergeComplexHierarchyMultiple) {
  resources::StyleInfo style_info;

  // Create a more complex hierarchy:
  //     root
  //    /    \
  //   A      B (B has 2 children, so no merge)
  //   |     / \
  //   C    D   E
  //   |        |
  //   F        G
  //            |
  //            H

  uint32_t root_id = 0x7f010001;
  uint32_t a_id = 0x7f010002;
  uint32_t b_id = 0x7f010003;
  uint32_t c_id = 0x7f010004;
  uint32_t d_id = 0x7f010005;
  uint32_t e_id = 0x7f010006;
  uint32_t f_id = 0x7f010007;
  uint32_t g_id = 0x7f010008;
  uint32_t h_id = 0x7f010009;

  auto root_vertex = add_vertex(style_info, root_id);
  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);
  auto f_vertex = add_vertex(style_info, f_id);
  auto g_vertex = add_vertex(style_info, g_id);
  auto h_vertex = add_vertex(style_info, h_id);

  add_edge(style_info, root_vertex, a_vertex);
  add_edge(style_info, root_vertex, b_vertex);
  add_edge(style_info, a_vertex, c_vertex);
  add_edge(style_info, b_vertex, d_vertex);
  add_edge(style_info, b_vertex, e_vertex);
  add_edge(style_info, c_vertex, f_vertex);
  add_edge(style_info, e_vertex, g_vertex);
  add_edge(style_info, g_vertex, h_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Expected merges: (A, C), (C, F), (E, G), (G, H)
  // Root has 2 children, so no merge
  EXPECT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].size(), 2);
  EXPECT_EQ(result[1].size(), 2);

  EXPECT_THAT(result[0], ::testing::ElementsAre(a_id, c_id));
  EXPECT_THAT(result[1], ::testing::ElementsAre(e_id, g_id));
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeLeafNodesOnly) {
  resources::StyleInfo style_info;

  uint32_t node1_id = 0x7f010001;
  uint32_t node2_id = 0x7f010002;
  uint32_t node3_id = 0x7f010003;

  add_vertex(style_info, node1_id);
  add_vertex(style_info, node2_id);
  add_vertex(style_info, node3_id);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  EXPECT_EQ(result.size(), 0);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeEmptyGraph) {
  resources::StyleInfo style_info;

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  EXPECT_EQ(result.size(), 0);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeLongChainPreOrder) {
  resources::StyleInfo style_info;

  // Create a long chain: A -> B -> C -> D -> E
  // Each node has exactly one child (except E)
  uint32_t a_id = 0x7f010001;
  uint32_t b_id = 0x7f010002;
  uint32_t c_id = 0x7f010003;
  uint32_t d_id = 0x7f010004;
  uint32_t e_id = 0x7f010005;

  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);

  add_edge(style_info, a_vertex, b_vertex);
  add_edge(style_info, b_vertex, c_vertex);
  add_edge(style_info, c_vertex, d_vertex);
  add_edge(style_info, d_vertex, e_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Should have 4 merge pairs: (A,B), (B,C), (C,D), (D,E)
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].size(), 4);

  EXPECT_THAT(result[0], ::testing::ElementsAre(a_id, b_id, c_id, d_id));
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeMixedConstraints) {
  resources::StyleInfo style_info;

  // Create hierarchy with mixed constraints:
  //     root
  //    /    \
  //   A      B (ambiguous)
  //   |      |
  //   C      D (directly reachable)
  //   |      |
  //   E      F

  uint32_t root_id = 0x7f010001;
  uint32_t a_id = 0x7f010002;
  uint32_t b_id = 0x7f010003;
  uint32_t c_id = 0x7f010004;
  uint32_t d_id = 0x7f010005;
  uint32_t e_id = 0x7f010006;
  uint32_t f_id = 0x7f010007;

  auto root_vertex = add_vertex(style_info, root_id);
  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);
  auto f_vertex = add_vertex(style_info, f_id);

  add_edge(style_info, root_vertex, a_vertex);
  add_edge(style_info, root_vertex, b_vertex);
  add_edge(style_info, a_vertex, c_vertex);
  add_edge(style_info, b_vertex, d_vertex);
  add_edge(style_info, c_vertex, e_vertex);
  add_edge(style_info, d_vertex, f_vertex);

  UnorderedSet<uint32_t> ambiguous_styles = {b_id};
  UnorderedSet<uint32_t> directly_reachable_styles = {d_id};

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Expected merges: (A, C) and (C, E)
  // Root has 2 children, so no merge
  // B is ambiguous, so no merge for B->D
  // D is directly reachable, so no merge for D->F
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].size(), 2);

  // Verify the correct pairs are present in pre-order
  EXPECT_THAT(result[0], ::testing::ElementsAre(a_id, c_id));
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeUnderReachable) {
  resources::StyleInfo style_info;

  // Create hierarchy with mixed constraints:
  //     root
  //    /    \
  //   A      B (reachable)
  //   |      |
  //   C      D
  //   |      |
  //   E      F

  uint32_t root_id = 0x7f010001;
  uint32_t a_id = 0x7f010002;
  uint32_t b_id = 0x7f010003;
  uint32_t c_id = 0x7f010004;
  uint32_t d_id = 0x7f010005;
  uint32_t e_id = 0x7f010006;
  uint32_t f_id = 0x7f010007;

  auto root_vertex = add_vertex(style_info, root_id);
  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);
  auto f_vertex = add_vertex(style_info, f_id);

  add_edge(style_info, root_vertex, a_vertex);
  add_edge(style_info, root_vertex, b_vertex);
  add_edge(style_info, a_vertex, c_vertex);
  add_edge(style_info, b_vertex, d_vertex);
  add_edge(style_info, c_vertex, e_vertex);
  add_edge(style_info, d_vertex, f_vertex);

  UnorderedSet<uint32_t> ambiguous_styles;
  UnorderedSet<uint32_t> directly_reachable_styles = {b_id};

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Expected merges: (A, C) and (C, E)
  // Root has 2 children, so no merge
  // B is ambiguous, so no merge for B->D
  // D is directly reachable, so no merge for D->F
  EXPECT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].size(), 2);
  EXPECT_EQ(result[1].size(), 1);

  // Verify the correct pairs are present in pre-order
  EXPECT_THAT(result[0], ::testing::ElementsAre(a_id, c_id));
  EXPECT_EQ(result[1][0], d_id);
}

TEST_F(ResourceValueMergingPassTest, GetResourcesToMergeWithAmbiguousTail) {
  resources::StyleInfo style_info;

  // Create a chain: grandparent -> parent -> child
  // where both grandparent and parent have exactly one child
  uint32_t grandparent_id = 0x7f010001;
  uint32_t parent_id = 0x7f010002;
  uint32_t child_id = 0x7f010003;

  auto grandparent_vertex = add_vertex(style_info, grandparent_id);
  auto parent_vertex = add_vertex(style_info, parent_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, grandparent_vertex, parent_vertex);
  add_edge(style_info, parent_vertex, child_vertex);

  UnorderedSet<uint32_t> ambiguous_styles = {child_id};
  UnorderedSet<uint32_t> directly_reachable_styles;

  auto result = m_pass.get_resources_to_merge(style_info, ambiguous_styles,
                                              directly_reachable_styles);

  // Should have two merge pairs: (grandparent, parent) and (parent, child)
  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].size(), 1);

  // Verify pre-order traversal: grandparent->parent should come before
  // parent->child
  EXPECT_THAT(result[0], ::testing::ElementsAre(grandparent_id));
}

TEST_F(ResourceValueMergingPassTest,
       GetParentAndAttributeModificationsForMergingBasic) {
  resources::StyleInfo style_info;

  // Create a chain of resources: parent -> middle -> child
  uint32_t parent_id = 0x7f010001;
  uint32_t middle_id = 0x7f010002;
  uint32_t child_id = 0x7f010003;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;
  uint32_t attr_id3 = 0x7f020003;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto middle_vertex = add_vertex(style_info, middle_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, parent_vertex, middle_vertex);
  add_edge(style_info, middle_vertex, child_vertex);

  resources::StyleResource parent_style;
  parent_style.id = parent_id;
  parent_style.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  parent_style.attributes.insert({attr_id1, value1});
  style_info.styles[parent_id].push_back(parent_style);

  resources::StyleResource middle_style;
  middle_style.id = middle_id;
  middle_style.parent = parent_id;
  resources::StyleResource::Value value2(43, 0);
  middle_style.attributes.insert({attr_id2, value2});
  style_info.styles[middle_id].push_back(middle_style);

  resources::StyleResource child_style;
  child_style.id = child_id;
  child_style.parent = middle_id;
  resources::StyleResource::Value value3(44, 0);
  child_style.attributes.insert({attr_id3, value3});
  style_info.styles[child_id].push_back(child_style);

  std::vector<uint32_t> resources_to_merge = {parent_id, middle_id};

  auto modification = m_pass.get_parent_and_attribute_modifications_for_merging(
      style_info, resources_to_merge);

  EXPECT_EQ(modification.resource_id, child_id);
  EXPECT_EQ(modification.parent_id, 0);
  EXPECT_EQ(modification.values.size(), 2);

  auto attr1_it = modification.values.find(attr_id1);
  auto attr2_it = modification.values.find(attr_id2);
  EXPECT_TRUE(attr1_it != modification.values.end());
  EXPECT_TRUE(attr2_it != modification.values.end());
}

TEST_F(ResourceValueMergingPassTest,
       GetParentAndAttributeModificationsForMergingOverrideAttributes) {
  resources::StyleInfo style_info;

  // Create a chain of resources: parent -> middle -> child
  uint32_t parent_id = 0x7f010001;
  uint32_t middle_id = 0x7f010002;
  uint32_t child_id = 0x7f010003;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  auto parent_vertex = add_vertex(style_info, parent_id);
  auto middle_vertex = add_vertex(style_info, middle_id);
  auto child_vertex = add_vertex(style_info, child_id);

  add_edge(style_info, parent_vertex, middle_vertex);
  add_edge(style_info, middle_vertex, child_vertex);

  resources::StyleResource parent_style;
  parent_style.id = parent_id;
  parent_style.parent = 0;
  resources::StyleResource::Value value1(42, 0);
  parent_style.attributes.insert({attr_id1, value1});
  style_info.styles[parent_id].push_back(parent_style);

  resources::StyleResource middle_style;
  middle_style.id = middle_id;
  middle_style.parent = parent_id;
  resources::StyleResource::Value value2(43, 0);
  middle_style.attributes.insert({attr_id1, value2});
  middle_style.attributes.insert({attr_id2, value2});
  style_info.styles[middle_id].push_back(middle_style);

  resources::StyleResource child_style;
  child_style.id = child_id;
  child_style.parent = middle_id;
  style_info.styles[child_id].push_back(child_style);

  // Resources to merge: parent and middle
  std::vector<uint32_t> resources_to_merge = {parent_id, middle_id};

  auto modification = m_pass.get_parent_and_attribute_modifications_for_merging(
      style_info, resources_to_merge);

  EXPECT_EQ(modification.resource_id, child_id);
  EXPECT_EQ(modification.parent_id, 0);
  EXPECT_EQ(modification.values.size(), 2);

  auto attr1_it = modification.values.find(attr_id1);
  auto attr2_it = modification.values.find(attr_id2);
  EXPECT_TRUE(attr1_it != modification.values.end());
  EXPECT_TRUE(attr2_it != modification.values.end());
  EXPECT_EQ(attr1_it->second, value2);
  EXPECT_EQ(attr2_it->second, value2);
}

TEST_F(ResourceValueMergingPassTest,
       GetParentAndAttributeModificationsForMergingLongChain) {
  resources::StyleInfo style_info;

  // Create a long chain: A -> B -> C -> D -> E
  uint32_t a_id = 0x7f010001;
  uint32_t b_id = 0x7f010002;
  uint32_t c_id = 0x7f010003;
  uint32_t d_id = 0x7f010004;
  uint32_t e_id = 0x7f010005;

  uint32_t attr_a = 0x7f020001;
  uint32_t attr_b = 0x7f020002;
  uint32_t attr_c = 0x7f020003;
  uint32_t attr_d = 0x7f020004;

  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);

  add_edge(style_info, a_vertex, b_vertex);
  add_edge(style_info, b_vertex, c_vertex);
  add_edge(style_info, c_vertex, d_vertex);
  add_edge(style_info, d_vertex, e_vertex);

  resources::StyleResource a_style;
  a_style.id = a_id;
  a_style.parent = 0;
  resources::StyleResource::Value value_a(10, 0);
  a_style.attributes.insert({attr_a, value_a});
  style_info.styles[a_id].push_back(a_style);

  resources::StyleResource b_style;
  b_style.id = b_id;
  b_style.parent = a_id;
  resources::StyleResource::Value value_b(20, 0);
  b_style.attributes.insert({attr_b, value_b});
  style_info.styles[b_id].push_back(b_style);

  resources::StyleResource c_style;
  c_style.id = c_id;
  c_style.parent = b_id;
  resources::StyleResource::Value value_c(30, 0);
  c_style.attributes.insert({attr_c, value_c});
  style_info.styles[c_id].push_back(c_style);

  resources::StyleResource d_style;
  d_style.id = d_id;
  d_style.parent = c_id;
  resources::StyleResource::Value value_d(40, 0);
  d_style.attributes.insert({attr_d, value_d});
  style_info.styles[d_id].push_back(d_style);

  resources::StyleResource e_style;
  e_style.id = e_id;
  e_style.parent = d_id;
  style_info.styles[e_id].push_back(e_style);

  // Test merging the entire chain A -> B -> C -> D
  std::vector<uint32_t> resources_to_merge = {a_id, b_id, c_id, d_id};

  // Call the function under test
  auto modification = m_pass.get_parent_and_attribute_modifications_for_merging(
      style_info, resources_to_merge);

  // Verify the results
  EXPECT_EQ(modification.resource_id, e_id);
  EXPECT_EQ(modification.parent_id, 0);
  EXPECT_EQ(modification.values.size(), 4);

  // Check that all attributes from the chain are present
  auto attr_a_it = modification.values.find(attr_a);
  auto attr_b_it = modification.values.find(attr_b);
  auto attr_c_it = modification.values.find(attr_c);
  auto attr_d_it = modification.values.find(attr_d);

  EXPECT_TRUE(attr_a_it != modification.values.end());
  EXPECT_TRUE(attr_b_it != modification.values.end());
  EXPECT_TRUE(attr_c_it != modification.values.end());
  EXPECT_TRUE(attr_d_it != modification.values.end());

  EXPECT_EQ(attr_a_it->second, value_a);
  EXPECT_EQ(attr_b_it->second, value_b);
  EXPECT_EQ(attr_c_it->second, value_c);
  EXPECT_EQ(attr_d_it->second, value_d);
}

TEST_F(ResourceValueMergingPassTest,
       GetParentAndAttributeModificationsForMergingLongChainWithOverrides) {
  resources::StyleInfo style_info;

  // Create a long chain: A -> B -> C -> D -> E
  uint32_t a_id = 0x7f010001;
  uint32_t b_id = 0x7f010002;
  uint32_t c_id = 0x7f010003;
  uint32_t d_id = 0x7f010004;
  uint32_t e_id = 0x7f010005;

  uint32_t common_attr = 0x7f020001;
  uint32_t unique_attr = 0x7f020002;

  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);

  add_edge(style_info, a_vertex, b_vertex);
  add_edge(style_info, b_vertex, c_vertex);
  add_edge(style_info, c_vertex, d_vertex);
  add_edge(style_info, d_vertex, e_vertex);

  resources::StyleResource a_style;
  a_style.id = a_id;
  a_style.parent = 0;
  resources::StyleResource::Value value_a(10, 0);
  a_style.attributes.insert({common_attr, value_a});
  style_info.styles[a_id].push_back(a_style);

  resources::StyleResource b_style;
  b_style.id = b_id;
  b_style.parent = a_id;
  resources::StyleResource::Value value_b(20, 0);
  b_style.attributes.insert({common_attr, value_b});
  style_info.styles[b_id].push_back(b_style);

  resources::StyleResource c_style;
  c_style.id = c_id;
  c_style.parent = b_id;
  resources::StyleResource::Value value_c(30, 0);
  c_style.attributes.insert({common_attr, value_c});
  resources::StyleResource::Value value_unique(100, 0);
  c_style.attributes.insert({unique_attr, value_unique});
  style_info.styles[c_id].push_back(c_style);

  resources::StyleResource d_style;
  d_style.id = d_id;
  d_style.parent = c_id;
  resources::StyleResource::Value value_d(40, 0);
  d_style.attributes.insert({common_attr, value_d});
  style_info.styles[d_id].push_back(d_style);

  resources::StyleResource e_style;
  e_style.id = e_id;
  e_style.parent = d_id;
  style_info.styles[e_id].push_back(e_style);

  // Test merging the entire chain B -> C -> D
  std::vector<uint32_t> resources_to_merge = {b_id, c_id, d_id};

  auto modification = m_pass.get_parent_and_attribute_modifications_for_merging(
      style_info, resources_to_merge);

  EXPECT_EQ(modification.resource_id, e_id);
  EXPECT_EQ(modification.parent_id, a_id);
  EXPECT_EQ(modification.values.size(), 2);

  auto common_attr_it = modification.values.find(common_attr);
  auto unique_attr_it = modification.values.find(unique_attr);

  EXPECT_TRUE(common_attr_it != modification.values.end());
  EXPECT_TRUE(unique_attr_it != modification.values.end());

  EXPECT_EQ(common_attr_it->second, value_d);
  EXPECT_EQ(unique_attr_it->second, value_unique);
}

TEST_F(ResourceValueMergingPassTest,
       GetParentAndAttributeModificationsForMergingVeryLongChainSingleValue) {
  resources::StyleInfo style_info;

  // Create a very long chain: A -> B -> C -> D -> E -> F -> G -> H
  uint32_t a_id = 0x7f010001;
  uint32_t b_id = 0x7f010002;
  uint32_t c_id = 0x7f010003;
  uint32_t d_id = 0x7f010004;
  uint32_t e_id = 0x7f010005;
  uint32_t f_id = 0x7f010006;
  uint32_t g_id = 0x7f010007;
  uint32_t h_id = 0x7f010008;

  uint32_t single_attr = 0x7f020001;

  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);
  auto f_vertex = add_vertex(style_info, f_id);
  auto g_vertex = add_vertex(style_info, g_id);
  auto h_vertex = add_vertex(style_info, h_id);

  add_edge(style_info, a_vertex, b_vertex);
  add_edge(style_info, b_vertex, c_vertex);
  add_edge(style_info, c_vertex, d_vertex);
  add_edge(style_info, d_vertex, e_vertex);
  add_edge(style_info, e_vertex, f_vertex);
  add_edge(style_info, f_vertex, g_vertex);
  add_edge(style_info, g_vertex, h_vertex);

  resources::StyleResource a_style;
  a_style.id = a_id;
  a_style.parent = 0;
  resources::StyleResource::Value value_a(10, 0);
  a_style.attributes.insert({single_attr, value_a});
  style_info.styles[a_id].push_back(a_style);

  resources::StyleResource b_style;
  b_style.id = b_id;
  b_style.parent = a_id;
  resources::StyleResource::Value value_b(20, 0);
  b_style.attributes.insert({single_attr, value_b});
  style_info.styles[b_id].push_back(b_style);

  resources::StyleResource c_style;
  c_style.id = c_id;
  c_style.parent = b_id;
  resources::StyleResource::Value value_c(30, 0);
  c_style.attributes.insert({single_attr, value_c});
  style_info.styles[c_id].push_back(c_style);

  resources::StyleResource d_style;
  d_style.id = d_id;
  d_style.parent = c_id;
  resources::StyleResource::Value value_d(40, 0);
  d_style.attributes.insert({single_attr, value_d});
  style_info.styles[d_id].push_back(d_style);

  resources::StyleResource e_style;
  e_style.id = e_id;
  e_style.parent = d_id;
  resources::StyleResource::Value value_e(50, 0);
  e_style.attributes.insert({single_attr, value_e});
  style_info.styles[e_id].push_back(e_style);

  resources::StyleResource f_style;
  f_style.id = f_id;
  f_style.parent = e_id;
  resources::StyleResource::Value value_f(60, 0);
  f_style.attributes.insert({single_attr, value_f});
  style_info.styles[f_id].push_back(f_style);

  resources::StyleResource g_style;
  g_style.id = g_id;
  g_style.parent = f_id;
  resources::StyleResource::Value value_g(70, 0);
  g_style.attributes.insert({single_attr, value_g});
  style_info.styles[g_id].push_back(g_style);

  resources::StyleResource h_style;
  h_style.id = h_id;
  h_style.parent = g_id;
  style_info.styles[h_id].push_back(h_style);

  // Test merging the entire chain A -> B -> C -> D -> E -> F -> G
  std::vector<uint32_t> resources_to_merge = {a_id, b_id, c_id, d_id,
                                              e_id, f_id, g_id};

  auto modification = m_pass.get_parent_and_attribute_modifications_for_merging(
      style_info, resources_to_merge);

  EXPECT_EQ(modification.resource_id, h_id);
  EXPECT_EQ(modification.parent_id, 0);
  EXPECT_EQ(modification.values.size(), 1);

  auto attr_it = modification.values.find(single_attr);
  EXPECT_TRUE(attr_it != modification.values.end());

  EXPECT_EQ(attr_it->second, value_g);
}

TEST_F(ResourceValueMergingPassTest,
       GetParentAndAttributeModificationsForMergingPartialChain) {
  resources::StyleInfo style_info;

  // Create a long chain: A -> B -> C -> D -> E -> F -> G -> H
  uint32_t a_id = 0x7f010001;
  uint32_t b_id = 0x7f010002;
  uint32_t c_id = 0x7f010003;
  uint32_t d_id = 0x7f010004;
  uint32_t e_id = 0x7f010005;
  uint32_t f_id = 0x7f010006;
  uint32_t g_id = 0x7f010007;
  uint32_t h_id = 0x7f010008;

  uint32_t single_attr = 0x7f020001;

  auto a_vertex = add_vertex(style_info, a_id);
  auto b_vertex = add_vertex(style_info, b_id);
  auto c_vertex = add_vertex(style_info, c_id);
  auto d_vertex = add_vertex(style_info, d_id);
  auto e_vertex = add_vertex(style_info, e_id);
  auto f_vertex = add_vertex(style_info, f_id);
  auto g_vertex = add_vertex(style_info, g_id);
  auto h_vertex = add_vertex(style_info, h_id);

  add_edge(style_info, a_vertex, b_vertex);
  add_edge(style_info, b_vertex, c_vertex);
  add_edge(style_info, c_vertex, d_vertex);
  add_edge(style_info, d_vertex, e_vertex);
  add_edge(style_info, e_vertex, f_vertex);
  add_edge(style_info, f_vertex, g_vertex);
  add_edge(style_info, g_vertex, h_vertex);

  resources::StyleResource a_style;
  a_style.id = a_id;
  a_style.parent = 0;
  resources::StyleResource::Value value_a(10, 0);
  a_style.attributes.insert({single_attr, value_a});
  style_info.styles[a_id].push_back(a_style);

  resources::StyleResource b_style;
  b_style.id = b_id;
  b_style.parent = a_id;
  resources::StyleResource::Value value_b(20, 0);
  b_style.attributes.insert({single_attr, value_b});
  style_info.styles[b_id].push_back(b_style);

  resources::StyleResource c_style;
  c_style.id = c_id;
  c_style.parent = b_id;
  resources::StyleResource::Value value_c(30, 0);
  c_style.attributes.insert({single_attr, value_c});
  style_info.styles[c_id].push_back(c_style);

  resources::StyleResource d_style;
  d_style.id = d_id;
  d_style.parent = c_id;
  resources::StyleResource::Value value_d(40, 0);
  d_style.attributes.insert({single_attr, value_d});
  style_info.styles[d_id].push_back(d_style);

  resources::StyleResource e_style;
  e_style.id = e_id;
  e_style.parent = d_id;
  resources::StyleResource::Value value_e(50, 0);
  e_style.attributes.insert({single_attr, value_e});
  style_info.styles[e_id].push_back(e_style);

  resources::StyleResource f_style;
  f_style.id = f_id;
  f_style.parent = e_id;
  resources::StyleResource::Value value_f(60, 0);
  f_style.attributes.insert({single_attr, value_f});
  style_info.styles[f_id].push_back(f_style);

  resources::StyleResource g_style;
  g_style.id = g_id;
  g_style.parent = f_id;
  resources::StyleResource::Value value_g(70, 0);
  g_style.attributes.insert({single_attr, value_g});
  style_info.styles[g_id].push_back(g_style);

  resources::StyleResource h_style;
  h_style.id = h_id;
  h_style.parent = g_id;
  style_info.styles[h_id].push_back(h_style);

  // Test merging just a portion of the chain: C -> D -> E
  std::vector<uint32_t> resources_to_merge = {c_id, d_id, e_id};

  auto modification = m_pass.get_parent_and_attribute_modifications_for_merging(
      style_info, resources_to_merge);

  EXPECT_EQ(modification.resource_id, f_id);
  EXPECT_EQ(modification.parent_id, b_id);
  EXPECT_EQ(modification.values.size(), 1);

  auto attr_it = modification.values.find(single_attr);
  EXPECT_TRUE(attr_it != modification.values.end());

  EXPECT_EQ(attr_it->second, value_e);
}

TEST_F(ResourceValueMergingPassTest,
       ShouldCreateSyntheticResourcesCheaperSynthetic) {
  uint32_t synthetic_style_cost = 100;
  uint32_t total_attribute_references = 10;
  uint32_t attribute_count = 1;

  bool result = m_pass.should_create_synthetic_resources(
      synthetic_style_cost, total_attribute_references, attribute_count);

  EXPECT_TRUE(result);
}

TEST_F(ResourceValueMergingPassTest,
       ShouldCreateSyntheticResourcesMultipleAttributes) {
  uint32_t synthetic_style_cost = 100;
  uint32_t total_attribute_references = 3;
  uint32_t attribute_count = 3;

  bool result = m_pass.should_create_synthetic_resources(
      synthetic_style_cost + 20, total_attribute_references, attribute_count);

  EXPECT_FALSE(result);
}

TEST_F(ResourceValueMergingPassTest,
       ShouldCreateSyntheticResourcesMoreExpensiveSynthetic) {
  uint32_t synthetic_style_cost = 200;
  uint32_t total_attribute_references = 5;
  uint32_t attribute_count = 3;

  bool result = m_pass.should_create_synthetic_resources(
      synthetic_style_cost + 20, total_attribute_references, attribute_count);

  EXPECT_FALSE(result);
}

TEST_F(ResourceValueMergingPassTest, ShouldCreateSyntheticResourcesEqualCost) {
  uint32_t synthetic_style_cost = 100;
  uint32_t total_attribute_references = 10;
  uint32_t attribute_count = 1;

  bool result = m_pass.should_create_synthetic_resources(
      synthetic_style_cost + 20, total_attribute_references, attribute_count);

  EXPECT_FALSE(result);
}

TEST_F(ResourceValueMergingPassTest, FindIntraGraphHoistingsEmptyStyleInfo) {
  resources::StyleInfo style_info;
  UnorderedSet<uint32_t> directly_reachable_styles;
  UnorderedSet<uint32_t> ambiguous_styles;

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest,
       FindIntraGraphHoistingsEmptyDirectlyReachable) {
  resources::StyleInfo style_info;

  uint32_t root_id = 0x7f010001;
  add_vertex(style_info, root_id);

  UnorderedSet<uint32_t> directly_reachable_styles;
  UnorderedSet<uint32_t> ambiguous_styles;

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest,
       FindIntraGraphHoistingsSingleDirectlyReachable) {
  resources::StyleInfo style_info;

  uint32_t root_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;
  uint32_t attr_id = 0x7f020001;

  auto root_vertex = add_vertex(style_info, root_id);
  auto child_vertex = add_vertex(style_info, child_id);
  add_edge(style_info, root_vertex, child_vertex);

  resources::StyleResource root_style;
  root_style.id = root_id;
  root_style.parent = 0;
  style_info.styles[root_id].push_back(root_style);

  resources::StyleResource child_style;
  child_style.id = child_id;
  child_style.parent = root_id;
  resources::StyleResource::Value value(42, 0);
  child_style.attributes.insert({attr_id, value});
  style_info.styles[child_id].push_back(child_style);

  UnorderedSet<uint32_t> directly_reachable_styles = {root_id};
  UnorderedSet<uint32_t> ambiguous_styles;

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  EXPECT_THAT(result[0], ::testing::UnorderedElementsAre(child_id));
}

TEST_F(ResourceValueMergingPassTest,
       FindIntraGraphHoistingsChildDirectlyReachable) {
  resources::StyleInfo style_info;

  uint32_t root_id = 0x7f010001;
  uint32_t child1_id = 0x7f010002;
  uint32_t child2_id = 0x7f010003;
  uint32_t attr_id = 0x7f020001;

  auto root_vertex = add_vertex(style_info, root_id);
  auto child1_vertex = add_vertex(style_info, child1_id);
  auto child2_vertex = add_vertex(style_info, child2_id);
  add_edge(style_info, root_vertex, child1_vertex);
  add_edge(style_info, root_vertex, child2_vertex);

  resources::StyleResource root_style;
  root_style.id = root_id;
  root_style.parent = 0;
  style_info.styles[root_id].push_back(root_style);

  resources::StyleResource::Value value(42, 0);

  resources::StyleResource child1_style;
  child1_style.id = child1_id;
  child1_style.parent = root_id;
  child1_style.attributes.insert({attr_id, value});
  style_info.styles[child1_id].push_back(child1_style);

  resources::StyleResource child2_style;
  child2_style.id = child2_id;
  child2_style.parent = root_id;
  child2_style.attributes.insert({attr_id, value});
  style_info.styles[child2_id].push_back(child2_style);

  // Test case 1: Child directly reachable
  {
    UnorderedSet<uint32_t> directly_reachable_styles = {root_id, child1_id};
    UnorderedSet<uint32_t> ambiguous_styles;

    auto result = m_pass.find_intra_graph_hoistings(
        style_info, directly_reachable_styles, ambiguous_styles);

    EXPECT_THAT(result.size(), 1);
    EXPECT_THAT(result[0],
                ::testing::UnorderedElementsAre(child1_id, child2_id));
  }

  // Test case 2: Ambiguous root
  {
    UnorderedSet<uint32_t> directly_reachable_styles = {root_id};
    UnorderedSet<uint32_t> ambiguous_styles = {root_id};

    auto result = m_pass.find_intra_graph_hoistings(
        style_info, directly_reachable_styles, ambiguous_styles);

    EXPECT_TRUE(result.empty());
  }
}

TEST_F(ResourceValueMergingPassTest, FindIntraGraphHoistingsComplexHierarchy) {
  resources::StyleInfo style_info;

  uint32_t root_id = 0x7f010001;
  uint32_t child1_id = 0x7f010002;
  uint32_t child2_id = 0x7f010003;
  uint32_t grandchild1_id = 0x7f010004;
  uint32_t grandchild2_id = 0x7f010005;
  uint32_t attr_id1 = 0x7f020001;
  uint32_t attr_id2 = 0x7f020002;

  auto root_vertex = add_vertex(style_info, root_id);
  auto child1_vertex = add_vertex(style_info, child1_id);
  auto child2_vertex = add_vertex(style_info, child2_id);
  auto grandchild1_vertex = add_vertex(style_info, grandchild1_id);
  auto grandchild2_vertex = add_vertex(style_info, grandchild2_id);

  add_edge(style_info, root_vertex, child1_vertex);
  add_edge(style_info, root_vertex, child2_vertex);
  add_edge(style_info, child1_vertex, grandchild1_vertex);
  add_edge(style_info, child2_vertex, grandchild2_vertex);

  resources::StyleResource root_style;
  root_style.id = root_id;
  root_style.parent = 0;
  style_info.styles[root_id].push_back(root_style);

  resources::StyleResource child1_style;
  child1_style.id = child1_id;
  child1_style.parent = root_id;
  style_info.styles[child1_id].push_back(child1_style);

  resources::StyleResource child2_style;
  child2_style.id = child2_id;
  child2_style.parent = root_id;
  style_info.styles[child2_id].push_back(child2_style);

  // Grandchild1 has attr_id1 that can be hoisted to child1
  resources::StyleResource grandchild1_style;
  grandchild1_style.id = grandchild1_id;
  grandchild1_style.parent = child1_id;
  resources::StyleResource::Value value1(42, 0);
  grandchild1_style.attributes.insert({attr_id1, value1});
  style_info.styles[grandchild1_id].push_back(grandchild1_style);

  // Grandchild2 has attr_id2 that can be hoisted to child2
  resources::StyleResource grandchild2_style;
  grandchild2_style.id = grandchild2_id;
  grandchild2_style.parent = child2_id;
  resources::StyleResource::Value value2(43, 0);
  grandchild2_style.attributes.insert({attr_id2, value2});
  style_info.styles[grandchild2_id].push_back(grandchild2_style);

  UnorderedSet<uint32_t> directly_reachable_styles = {root_id, child1_id,
                                                      child2_id};
  UnorderedSet<uint32_t> ambiguous_styles;

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  EXPECT_THAT(result.size(), 2);
  EXPECT_THAT(result[0], ::testing::UnorderedElementsAre(grandchild1_id));
  EXPECT_THAT(result[1], ::testing::UnorderedElementsAre(grandchild2_id));
}

TEST_F(ResourceValueMergingPassTest,
       FindIntraGraphHoistingsWithAmbiguousStyles) {
  resources::StyleInfo style_info;

  uint32_t root_id = 0x7f010001;
  uint32_t child_id = 0x7f010002;
  uint32_t attr_id = 0x7f020001;

  auto root_vertex = add_vertex(style_info, root_id);
  auto child_vertex = add_vertex(style_info, child_id);
  add_edge(style_info, root_vertex, child_vertex);

  resources::StyleResource root_style;
  root_style.id = root_id;
  root_style.parent = 0;
  style_info.styles[root_id].push_back(root_style);

  resources::StyleResource child_style;
  child_style.id = child_id;
  child_style.parent = root_id;
  resources::StyleResource::Value value(42, 0);
  child_style.attributes.insert({attr_id, value});
  style_info.styles[child_id].push_back(child_style);

  UnorderedSet<uint32_t> directly_reachable_styles = {root_id};
  UnorderedSet<uint32_t> ambiguous_styles = {child_id};

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  // Since root is ambiguous, no hoisting should occur
  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest,
       FindIntraGraphHoistingsMixedChildrenStates) {
  resources::StyleInfo style_info;

  uint32_t root_id = 0x7f010001;
  uint32_t child1_id = 0x7f010002;
  uint32_t child2_id = 0x7f010003;
  uint32_t child3_id = 0x7f010004;
  uint32_t attr_id = 0x7f020001;

  auto root_vertex = add_vertex(style_info, root_id);
  auto child1_vertex = add_vertex(style_info, child1_id);
  auto child2_vertex = add_vertex(style_info, child2_id);
  auto child3_vertex = add_vertex(style_info, child3_id);

  add_edge(style_info, root_vertex, child1_vertex);
  add_edge(style_info, root_vertex, child2_vertex);
  add_edge(style_info, root_vertex, child3_vertex);

  resources::StyleResource root_style;
  root_style.id = root_id;
  root_style.parent = 0;
  style_info.styles[root_id].push_back(root_style);

  resources::StyleResource::Value value(42, 0);

  resources::StyleResource child1_style;
  child1_style.id = child1_id;
  child1_style.parent = root_id;
  child1_style.attributes.insert({attr_id, value});
  style_info.styles[child1_id].push_back(child1_style);

  resources::StyleResource child2_style;
  child2_style.id = child2_id;
  child2_style.parent = root_id;
  child2_style.attributes.insert({attr_id, value});
  style_info.styles[child2_id].push_back(child2_style);

  resources::StyleResource child3_style;
  child3_style.id = child3_id;
  child3_style.parent = root_id;
  child3_style.attributes.insert({attr_id, value});
  style_info.styles[child3_id].push_back(child3_style);

  UnorderedSet<uint32_t> directly_reachable_styles = {root_id};
  UnorderedSet<uint32_t> ambiguous_styles = {child2_id};

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  EXPECT_TRUE(result.empty());
}

TEST_F(ResourceValueMergingPassTest, FindIntraGraphHoistingsMultipleRoots) {
  resources::StyleInfo style_info;

  uint32_t root1_id = 0x7f010001;
  uint32_t root2_id = 0x7f010002;
  uint32_t child1_id = 0x7f010003;
  uint32_t child2_id = 0x7f010004;
  uint32_t attr_id = 0x7f020001;

  auto root1_vertex = add_vertex(style_info, root1_id);
  auto root2_vertex = add_vertex(style_info, root2_id);
  auto child1_vertex = add_vertex(style_info, child1_id);
  auto child2_vertex = add_vertex(style_info, child2_id);

  add_edge(style_info, root1_vertex, child1_vertex);
  add_edge(style_info, root2_vertex, child2_vertex);

  resources::StyleResource root1_style;
  root1_style.id = root1_id;
  root1_style.parent = 0;
  style_info.styles[root1_id].push_back(root1_style);

  resources::StyleResource root2_style;
  root2_style.id = root2_id;
  root2_style.parent = 0;
  style_info.styles[root2_id].push_back(root2_style);

  // Child1 has attribute that can be hoisted to root1
  resources::StyleResource child1_style;
  child1_style.id = child1_id;
  child1_style.parent = root1_id;
  resources::StyleResource::Value value(42, 0);
  child1_style.attributes.insert({attr_id, value});
  style_info.styles[child1_id].push_back(child1_style);

  // Child2 has attribute that can be hoisted to root2
  resources::StyleResource child2_style;
  child2_style.id = child2_id;
  child2_style.parent = root2_id;
  child2_style.attributes.insert({attr_id, value});
  style_info.styles[child2_id].push_back(child2_style);

  UnorderedSet<uint32_t> directly_reachable_styles = {root1_id, root2_id};
  UnorderedSet<uint32_t> ambiguous_styles;

  auto result = m_pass.find_intra_graph_hoistings(
      style_info, directly_reachable_styles, ambiguous_styles);

  EXPECT_THAT(result.size(), 2);
  EXPECT_THAT(result[0], ::testing::UnorderedElementsAre(child2_id));
  EXPECT_THAT(result[1], ::testing::UnorderedElementsAre(child1_id));
}
