/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
