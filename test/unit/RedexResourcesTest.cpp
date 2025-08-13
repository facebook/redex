/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "ApkResources.h"
#include "Debug.h"
#include "RedexResources.h"
#include "RedexTest.h"
#include "ResourcesTestDefs.h"
#include "androidfw/ResourceTypes.h"

using std::operator""sv;
using ::testing::UnorderedElementsAre;

TEST(RedexResources, ReadXmlTagsAndAttributes) {
  UnorderedSet<std::string> attributes_to_find;
  attributes_to_find.emplace("android:onClick");
  attributes_to_find.emplace("onClick");
  attributes_to_find.emplace("android:text");

  resources::StringOrReferenceSet classes;
  std::unordered_multimap<std::string, resources::StringOrReference>
      attribute_values;

  ApkResources resources("");
  resources.collect_layout_classes_and_attributes_for_file(
      get_env("test_layout_path"),
      attributes_to_find,
      &classes,
      &attribute_values);

  EXPECT_EQ(classes.size(), 3);
  EXPECT_EQ(count_strings(classes, "com.example.test.CustomViewGroup"), 1);
  EXPECT_EQ(count_strings(classes, "com.example.test.CustomTextView"), 1);
  EXPECT_EQ(count_strings(classes, "com.example.test.CustomButton"), 1);

  auto method_names =
      string_values_for_key(attribute_values, "android:onClick");
  EXPECT_EQ(method_names.size(), 2);
  EXPECT_EQ(method_names.count("fooClick"), 1);
  EXPECT_EQ(method_names.count("barClick"), 1);

  EXPECT_EQ(count_for_key(attribute_values, "android:text"), 4);
  EXPECT_EQ(count_for_key(attribute_values, "onClick"), 0);

  // Parse another file with slightly different form.
  resources::StringOrReferenceSet more_classes;
  std::unordered_multimap<std::string, resources::StringOrReference>
      more_attribute_values;
  resources.collect_layout_classes_and_attributes_for_file(
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
}

inline uint32_t to_uint(char c) {
  return static_cast<uint32_t>(static_cast<uint8_t>(c));
}

TEST(RedexResources, Mutf8Conversion) {
  bool be_noisy{false};
  auto verify = [&](std::u8string_view input,
                    const std::vector<uint8_t>& expected_bytes) {
    auto converted = resources::convert_utf8_to_mutf8(input);
    EXPECT_EQ(converted.size(), expected_bytes.size());
    size_t i = 0;
    for (auto c : converted) {
      EXPECT_EQ(static_cast<uint8_t>(c), expected_bytes[i++]);
      if (be_noisy) {
        std::cout << "GOT CHAR: 0x" << std::hex << to_uint(c) << std::dec
                  << std::endl;
      }
    }
    if (be_noisy) {
      std::cout << "Original:  " << std::string(input.begin(), input.end())
                << std::endl
                << "Converted: " << converted << std::endl;
    }
  };

  // Code points beyond U+FFFF
  verify(u8"Hello, \U0001F30E!",
         {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, 0xed, 0xa0, 0xbc, 0xed,
          0xbc, 0x8e, 0x21});
  verify(
      u8"\U0001F525\U0001F525",
      {0xed, 0xa0, 0xbd, 0xed, 0xb4, 0xa5, 0xed, 0xa0, 0xbd, 0xed, 0xb4, 0xa5});

  // Embedded null
  verify(u8"yo\0sup"sv, {0x79, 0x6f, 0xc0, 0x80, 0x73, 0x75, 0x70});

  // Regular UTF-8 string with one, two, three byte encoded code points that is
  // not changed
  verify(u8"e\u0205\u1E15", {0x65, 0xc8, 0x85, 0xe1, 0xb8, 0x95});
}

TEST(RedexResources, GetResourcesToKeep) {
  UnorderedSet<std::string> single_class_to_keep =
      resources::parse_keep_xml_file(get_env("single_resource_inclusion_path"));

  UnorderedSet<std::string> expected_single_class{"CronetProviderClassName"};

  EXPECT_EQ(expected_single_class, single_class_to_keep);

  UnorderedSet<std::string> expected_multiple_class{
      "CronetProviderClassName", "FooProviderClassName", "BarProviderClassName",
      "AnakinProviderClassName"};
  UnorderedSet<std::string> multiple_class_to_keep_and_spacing =
      resources::parse_keep_xml_file(
          get_env("multiple_resource_inclusion_path"));
  EXPECT_EQ(expected_multiple_class, multiple_class_to_keep_and_spacing);

  UnorderedSet<std::string> expected_empty_class{};
  UnorderedSet<std::string> empty_class_to_keep =
      resources::parse_keep_xml_file(get_env("empty_resource_inclusion_path"));
  EXPECT_EQ(expected_empty_class, empty_class_to_keep);
}

TEST(RedexResources, StyleResourceSpan) {
  resources::StyleResource::Value::Span span1{"bold", 0, 5};
  resources::StyleResource::Value::Span span2{"bold", 0, 5};
  EXPECT_TRUE(span1 == span2);

  resources::StyleResource::Value::Span span3{"italic", 0, 5};
  EXPECT_FALSE(span1 == span3);

  resources::StyleResource::Value::Span span4{"bold", 1, 5};
  EXPECT_FALSE(span1 == span4);

  resources::StyleResource::Value::Span span5{"bold", 0, 6};
  EXPECT_FALSE(span1 == span5);
}

TEST(RedexResources, StyleResourceValueBytes) {
  const uint8_t data_type = 1;
  const uint32_t bytes_value = 0x12345678;

  resources::StyleResource::Value value1(data_type, bytes_value);
  resources::StyleResource::Value value2(data_type, bytes_value);

  EXPECT_TRUE(value1 == value2);

  resources::StyleResource::Value value3(data_type + 1, bytes_value);
  EXPECT_FALSE(value1 == value3);

  resources::StyleResource::Value value4(data_type, bytes_value + 1);
  EXPECT_FALSE(value1 == value4);
}

TEST(RedexResources, StyleResourceValueString) {
  const uint8_t data_type = 2;
  const std::string str_value = "test string";

  resources::StyleResource::Value value1(data_type, str_value);
  resources::StyleResource::Value value2(data_type, str_value);

  EXPECT_TRUE(value1 == value2);

  resources::StyleResource::Value value3(data_type + 1, str_value);
  EXPECT_FALSE(value1 == value3);

  resources::StyleResource::Value value4(data_type, str_value + " modified");
  EXPECT_FALSE(value1 == value4);

  const std::string empty_str;
  resources::StyleResource::Value value5(data_type, empty_str);
  resources::StyleResource::Value value6(data_type, empty_str);
  EXPECT_TRUE(value5 == value6);
  EXPECT_FALSE(value1 == value5);
}

TEST(RedexResources, StyleResourceValueStyled) {
  const uint8_t data_type = 3;
  const std::string test_string = "Hello world!";
  std::vector<resources::StyleResource::Value::Span> spans1 = {
      {"bold", 0, 5}, {"italic", 6, 10}};

  resources::StyleResource::Value value1(data_type, test_string, spans1);

  std::vector<resources::StyleResource::Value::Span> spans2 = {
      {"bold", 0, 5}, {"italic", 6, 10}};
  resources::StyleResource::Value value2(data_type, test_string, spans2);

  EXPECT_TRUE(value1 == value2);

  resources::StyleResource::Value value3(data_type + 1, test_string, spans1);
  EXPECT_FALSE(value1 == value3);

  std::vector<resources::StyleResource::Value::Span> spans3 = {{"bold", 0, 5}};
  resources::StyleResource::Value value4(data_type, test_string, spans3);
  EXPECT_FALSE(value1 == value4);

  std::vector<resources::StyleResource::Value::Span> spans4 = {
      {"bold", 0, 5}, {"underline", 6, 10}};
  resources::StyleResource::Value value5(data_type, test_string, spans4);
  EXPECT_FALSE(value1 == value5);

  std::vector<resources::StyleResource::Value::Span> empty_spans;
  resources::StyleResource::Value value6(data_type, test_string, empty_spans);
  resources::StyleResource::Value value7(data_type, test_string, empty_spans);
  EXPECT_TRUE(value6 == value7);
  EXPECT_FALSE(value1 == value6);
}

TEST(RedexResources, StyleResourceValueMixedComparisons) {
  const uint8_t bytes_type = 1;
  const uint8_t string_type = 2;
  const uint8_t styled_type = 3;

  resources::StyleResource::Value bytes_value(bytes_type, 12345);
  resources::StyleResource::Value string_value(string_type, "test");

  std::vector<resources::StyleResource::Value::Span> spans = {{"bold", 0, 5}};
  resources::StyleResource::Value styled_value(
      styled_type, "styled text", spans);

  resources::StyleResource::Value another_bytes_value(bytes_type, 0);
  EXPECT_FALSE(bytes_value == string_value);
  EXPECT_FALSE(bytes_value == styled_value);
  EXPECT_FALSE(string_value == styled_value);

  resources::StyleResource::Value bytes_as_string_type(string_type, 12345);
  EXPECT_FALSE(bytes_value == bytes_as_string_type);
}

TEST(RedexResources, StyleResourceValueGetters) {
  const uint8_t bytes_type = 1;
  const uint32_t bytes_value = 0x12345678;
  resources::StyleResource::Value bytes_val(bytes_type, bytes_value);
  EXPECT_EQ(bytes_val.get_data_type(), bytes_type);
  EXPECT_EQ(bytes_val.get_value_bytes(), bytes_value);
  EXPECT_FALSE(bytes_val.get_value_string().has_value());
  EXPECT_TRUE(bytes_val.get_styled_string().empty());
  const uint8_t string_type = 2;
  const std::string str_value = "test string";
  resources::StyleResource::Value string_val(string_type, str_value);
  EXPECT_EQ(string_val.get_data_type(), string_type);
  EXPECT_EQ(string_val.get_value_bytes(), 0);
  EXPECT_TRUE(string_val.get_value_string().has_value());
  EXPECT_EQ(string_val.get_value_string().get(), str_value);
  EXPECT_TRUE(string_val.get_styled_string().empty());
  const uint8_t styled_type = 3;
  const std::string styled_string = "Hello world!";
  std::vector<resources::StyleResource::Value::Span> spans = {
      {"bold", 0, 5}, {"italic", 6, 10}};
  resources::StyleResource::Value styled_val(styled_type, styled_string, spans);
  EXPECT_EQ(styled_val.get_data_type(), styled_type);
  EXPECT_EQ(styled_val.get_value_bytes(), 0);
  EXPECT_TRUE(styled_val.get_value_string().has_value());
  EXPECT_EQ(styled_val.get_value_string().get(), styled_string);
  EXPECT_EQ(styled_val.get_styled_string().size(), 2);
  EXPECT_EQ(styled_val.get_styled_string()[0].tag, "bold");
  EXPECT_EQ(styled_val.get_styled_string()[0].first_char, 0);
  EXPECT_EQ(styled_val.get_styled_string()[0].last_char, 5);
  EXPECT_EQ(styled_val.get_styled_string()[1].tag, "italic");
  EXPECT_EQ(styled_val.get_styled_string()[1].first_char, 6);
  EXPECT_EQ(styled_val.get_styled_string()[1].last_char, 10);
}

TEST(RedexResources, StyleInfoGetRoots) {
  auto add_vertex = [](resources::StyleInfo& style_info, uint32_t id) {
    return boost::add_vertex(resources::StyleInfo::Node{id}, style_info.graph);
  };

  auto add_edge = [](resources::StyleInfo& style_info,
                     resources::StyleInfo::vertex_t parent,
                     resources::StyleInfo::vertex_t child) {
    boost::add_edge(parent, child, style_info.graph);
  };

  {
    resources::StyleInfo style_info;
    auto roots = style_info.get_roots();
    EXPECT_TRUE(roots.empty());
  }

  {
    resources::StyleInfo style_info;
    auto vertex = add_vertex(style_info, 0x7f010001);
    auto roots = style_info.get_roots();
    EXPECT_EQ(roots.size(), 1);
    EXPECT_TRUE(roots.find(vertex) != roots.end());
  }

  {
    resources::StyleInfo style_info;
    auto parent = add_vertex(style_info, 0x7f010001);
    auto child1 = add_vertex(style_info, 0x7f010002);
    auto child2 = add_vertex(style_info, 0x7f010003);

    add_edge(style_info, parent, child1);
    add_edge(style_info, parent, child2);

    auto roots = style_info.get_roots();

    EXPECT_EQ(roots.size(), 1);
    EXPECT_TRUE(roots.find(parent) != roots.end());
    EXPECT_TRUE(roots.find(child1) == roots.end());
    EXPECT_TRUE(roots.find(child2) == roots.end());
  }

  {
    resources::StyleInfo style_info;
    auto parent1 = add_vertex(style_info, 0x7f010001);
    auto parent2 = add_vertex(style_info, 0x7f010002);
    auto child1 = add_vertex(style_info, 0x7f010003);
    auto child2 = add_vertex(style_info, 0x7f010004);

    add_edge(style_info, parent1, child1);
    add_edge(style_info, parent2, child2);

    auto roots = style_info.get_roots();

    std::cout << "Test 4 roots size: " << roots.size() << std::endl;
    for (auto v : UnorderedIterable(roots)) {
      std::cout << "Root vertex: " << v
                << " with ID: " << style_info.graph[v].id << std::endl;
    }

    EXPECT_EQ(roots.size(), 2);
    EXPECT_TRUE(roots.find(parent1) != roots.end());
    EXPECT_TRUE(roots.find(parent2) != roots.end());
    EXPECT_TRUE(roots.find(child1) == roots.end());
    EXPECT_TRUE(roots.find(child2) == roots.end());
  }

  {
    resources::StyleInfo style_info;
    auto vertex1 = add_vertex(style_info, 0x7f010001);
    auto vertex2 = add_vertex(style_info, 0x7f010002);
    auto vertex3 = add_vertex(style_info, 0x7f010003);

    add_edge(style_info, vertex1, vertex2);
    add_edge(style_info, vertex2, vertex3);
    add_edge(style_info, vertex3, vertex1);

    auto roots = style_info.get_roots();
    EXPECT_TRUE(roots.empty());
  }
}

TEST(RedexResources, StyleInfoGetChildren) {
  const uint32_t SINGLE_NODE_ID = 0x7f010001;

  const uint32_t SIMPLE_PARENT_ID = 0x7f010001;
  const uint32_t SIMPLE_CHILD1_ID = 0x7f010002;
  const uint32_t SIMPLE_CHILD2_ID = 0x7f010003;

  const uint32_t MULTI_PARENT1_ID = 0x7f010001;
  const uint32_t MULTI_PARENT2_ID = 0x7f010002;
  const uint32_t MULTI_CHILD1_ID = 0x7f010003;
  const uint32_t MULTI_CHILD2_ID = 0x7f010004;
  const uint32_t MULTI_CHILD3_ID = 0x7f010005;

  const uint32_t TREE_ROOT_ID = 0x7f010001;
  const uint32_t TREE_MID1_ID = 0x7f010002;
  const uint32_t TREE_MID2_ID = 0x7f010003;
  const uint32_t TREE_LEAF1_ID = 0x7f010004;
  const uint32_t TREE_LEAF2_ID = 0x7f010005;
  const uint32_t TREE_LEAF3_ID = 0x7f010006;

  const uint32_t CYCLE_NODE1_ID = 0x7f010001;
  const uint32_t CYCLE_NODE2_ID = 0x7f010002;
  const uint32_t CYCLE_NODE3_ID = 0x7f010003;

  auto add_vertex = [](resources::StyleInfo& style_info, uint32_t id) {
    auto vertex =
        boost::add_vertex(resources::StyleInfo::Node{id}, style_info.graph);
    style_info.id_to_vertex[id] = vertex;
    return vertex;
  };

  auto add_edge = [](resources::StyleInfo& style_info,
                     resources::StyleInfo::vertex_t parent,
                     resources::StyleInfo::vertex_t child) {
    boost::add_edge(parent, child, style_info.graph);
  };

  {
    resources::StyleInfo style_info;
    EXPECT_THROW(style_info.get_children(SINGLE_NODE_ID), std::out_of_range);
  }

  {
    resources::StyleInfo style_info;
    add_vertex(style_info, SINGLE_NODE_ID);

    auto children = style_info.get_children(SINGLE_NODE_ID);
    EXPECT_TRUE(children.empty());
  }

  {
    resources::StyleInfo style_info;
    auto parent = add_vertex(style_info, SIMPLE_PARENT_ID);
    auto child1 = add_vertex(style_info, SIMPLE_CHILD1_ID);
    auto child2 = add_vertex(style_info, SIMPLE_CHILD2_ID);

    add_edge(style_info, parent, child1);
    add_edge(style_info, parent, child2);

    auto children = style_info.get_children(SIMPLE_PARENT_ID);
    EXPECT_EQ(children.size(), 2);
    EXPECT_THAT(children,
                UnorderedElementsAre(SIMPLE_CHILD1_ID, SIMPLE_CHILD2_ID));

    auto child1_children = style_info.get_children(SIMPLE_CHILD1_ID);
    EXPECT_TRUE(child1_children.empty());

    auto child2_children = style_info.get_children(SIMPLE_CHILD2_ID);
    EXPECT_TRUE(child2_children.empty());
  }

  {
    resources::StyleInfo style_info;
    auto parent1 = add_vertex(style_info, MULTI_PARENT1_ID);
    auto parent2 = add_vertex(style_info, MULTI_PARENT2_ID);
    auto child1 = add_vertex(style_info, MULTI_CHILD1_ID);
    auto child2 = add_vertex(style_info, MULTI_CHILD2_ID);
    auto child3 = add_vertex(style_info, MULTI_CHILD3_ID);

    add_edge(style_info, parent1, child1);
    add_edge(style_info, parent1, child2);
    add_edge(style_info, parent2, child3);

    auto parent1_children = style_info.get_children(MULTI_PARENT1_ID);
    EXPECT_EQ(parent1_children.size(), 2);
    EXPECT_THAT(parent1_children,
                UnorderedElementsAre(MULTI_CHILD1_ID, MULTI_CHILD2_ID));

    auto parent2_children = style_info.get_children(MULTI_PARENT2_ID);
    EXPECT_EQ(parent2_children.size(), 1);
    EXPECT_THAT(parent2_children, UnorderedElementsAre(MULTI_CHILD3_ID));
  }

  {
    resources::StyleInfo style_info;
    auto root = add_vertex(style_info, TREE_ROOT_ID);
    auto mid1 = add_vertex(style_info, TREE_MID1_ID);
    auto mid2 = add_vertex(style_info, TREE_MID2_ID);
    auto leaf1 = add_vertex(style_info, TREE_LEAF1_ID);
    auto leaf2 = add_vertex(style_info, TREE_LEAF2_ID);
    auto leaf3 = add_vertex(style_info, TREE_LEAF3_ID);

    add_edge(style_info, root, mid1);
    add_edge(style_info, root, mid2);
    add_edge(style_info, mid1, leaf1);
    add_edge(style_info, mid1, leaf2);
    add_edge(style_info, mid2, leaf3);

    auto root_children = style_info.get_children(TREE_ROOT_ID);
    EXPECT_EQ(root_children.size(), 2);
    EXPECT_THAT(root_children,
                UnorderedElementsAre(TREE_MID1_ID, TREE_MID2_ID));

    auto mid1_children = style_info.get_children(TREE_MID1_ID);
    EXPECT_EQ(mid1_children.size(), 2);
    EXPECT_THAT(mid1_children,
                UnorderedElementsAre(TREE_LEAF1_ID, TREE_LEAF2_ID));

    auto mid2_children = style_info.get_children(TREE_MID2_ID);
    EXPECT_EQ(mid2_children.size(), 1);
    EXPECT_THAT(mid2_children, UnorderedElementsAre(TREE_LEAF3_ID));

    auto leaf_children = style_info.get_children(TREE_LEAF1_ID);
    EXPECT_TRUE(leaf_children.empty());
  }

  {
    resources::StyleInfo style_info;
    auto vertex1 = add_vertex(style_info, CYCLE_NODE1_ID);
    auto vertex2 = add_vertex(style_info, CYCLE_NODE2_ID);
    auto vertex3 = add_vertex(style_info, CYCLE_NODE3_ID);

    add_edge(style_info, vertex1, vertex2);
    add_edge(style_info, vertex2, vertex3);
    add_edge(style_info, vertex3, vertex1);

    auto children1 = style_info.get_children(CYCLE_NODE1_ID);
    EXPECT_EQ(children1.size(), 1);
    EXPECT_THAT(children1, UnorderedElementsAre(CYCLE_NODE2_ID));

    auto children2 = style_info.get_children(CYCLE_NODE2_ID);
    EXPECT_EQ(children2.size(), 1);
    EXPECT_THAT(children2, UnorderedElementsAre(CYCLE_NODE3_ID));

    auto children3 = style_info.get_children(CYCLE_NODE3_ID);
    EXPECT_EQ(children3.size(), 1);
    EXPECT_THAT(children3, UnorderedElementsAre(CYCLE_NODE1_ID));
  }
}

TEST(RedexResources, StyleInfoDeepCopy) {
  auto add_vertex = [](resources::StyleInfo& style_info, uint32_t id) {
    return boost::add_vertex(resources::StyleInfo::Node{id}, style_info.graph);
  };

  auto add_edge = [](resources::StyleInfo& style_info,
                     resources::StyleInfo::vertex_t parent,
                     resources::StyleInfo::vertex_t child) {
    boost::add_edge(parent, child, style_info.graph);
  };

  resources::StyleInfo original;
  auto vertex1 = add_vertex(original, 0x7f010001);
  auto vertex2 = add_vertex(original, 0x7f010002);
  auto vertex3 = add_vertex(original, 0x7f010003);

  add_edge(original, vertex1, vertex2);
  add_edge(original, vertex1, vertex3);

  resources::StyleResource style_resource1;
  style_resource1.id = 0x7f010001;
  style_resource1.parent = 0x01010000;
  style_resource1.attributes.emplace(
      0x01010001, resources::StyleResource::Value(1, 0x12345678));
  style_resource1.attributes.emplace(
      0x01010002,
      resources::StyleResource::Value(2, std::string("test_value")));

  resources::StyleResource style_resource2;
  style_resource2.id = 0x7f010002;
  style_resource2.parent = 0x7f010001;
  style_resource2.attributes.emplace(
      0x01010003, resources::StyleResource::Value(1, 0x87654321));

  original.styles[0x7f010001] = {style_resource1};
  original.styles[0x7f010002] = {style_resource2};

  resources::StyleInfo copied(original);

  EXPECT_EQ(boost::num_vertices(original.graph),
            boost::num_vertices(copied.graph));
  EXPECT_EQ(boost::num_edges(original.graph), boost::num_edges(copied.graph));

  EXPECT_EQ(original.styles.size(), copied.styles.size());
  EXPECT_EQ(original.styles[0x7f010001].size(),
            copied.styles[0x7f010001].size());
  EXPECT_EQ(original.styles[0x7f010002].size(),
            copied.styles[0x7f010002].size());

  const auto& orig_style1 = original.styles[0x7f010001][0];
  const auto& copied_style1 = copied.styles[0x7f010001][0];
  EXPECT_EQ(orig_style1.id, copied_style1.id);
  EXPECT_EQ(orig_style1.parent, copied_style1.parent);
  EXPECT_EQ(orig_style1.attributes.size(), copied_style1.attributes.size());

  auto new_vertex = add_vertex(original, 0x7f010004);
  add_edge(original, vertex2, new_vertex);

  resources::StyleResource new_style_resource;
  new_style_resource.id = 0x7f010004;
  new_style_resource.parent = 0x7f010002;
  new_style_resource.attributes.emplace(
      0x01010004, resources::StyleResource::Value(1, 0xABCDEF00));
  original.styles[0x7f010004] = {new_style_resource};

  original.styles[0x7f010001][0].attributes.emplace(
      0x01010005,
      resources::StyleResource::Value(2, std::string("modified_value")));

  EXPECT_NE(boost::num_vertices(original.graph),
            boost::num_vertices(copied.graph));
  EXPECT_NE(boost::num_edges(original.graph), boost::num_edges(copied.graph));
  EXPECT_NE(original.styles.size(), copied.styles.size());

  EXPECT_EQ(boost::num_vertices(copied.graph), 3);
  EXPECT_EQ(boost::num_edges(copied.graph), 2);

  EXPECT_EQ(copied.styles.size(), 2);
  EXPECT_TRUE(copied.styles.find(0x7f010004) == copied.styles.end());
  EXPECT_TRUE(copied.styles[0x7f010001][0].attributes.find(0x01010005) ==
              copied.styles[0x7f010001][0].attributes.end());

  EXPECT_EQ(copied.styles.at(0x7f010001)[0]
                .attributes.at(0x01010001)
                .get_value_bytes(),
            0x12345678);
  EXPECT_EQ(copied.styles.at(0x7f010001)[0]
                .attributes.at(0x01010002)
                .get_value_string()
                .get(),
            "test_value");
  EXPECT_EQ(copied.styles.at(0x7f010002)[0]
                .attributes.at(0x01010003)
                .get_value_bytes(),
            0x87654321);
}

TEST(RedexResources, StyleInfoGetParent) {
  const uint32_t NONEXISTENT_ID = 0x7f020001;
  const uint32_t SINGLE_STYLE_ID = 0x7f020002;
  const uint32_t MULTI_STYLE_ID = 0x7f020003;
  const uint32_t NO_PARENT_ID = 0x7f020004;

  const uint32_t PARENT_ID = 0x7f010000;

  resources::StyleInfo style_info;

  auto parent1 = style_info.get_unambiguous_parent(NONEXISTENT_ID);
  EXPECT_FALSE(parent1.has_value());

  resources::StyleResource style_with_parent;
  style_with_parent.id = SINGLE_STYLE_ID;
  style_with_parent.parent = PARENT_ID;

  style_info.styles[SINGLE_STYLE_ID] = {style_with_parent};

  auto parent2 = style_info.get_unambiguous_parent(SINGLE_STYLE_ID);
  EXPECT_TRUE(parent2.has_value());
  EXPECT_EQ(parent2.value(), PARENT_ID);

  resources::StyleResource style1;
  style1.id = MULTI_STYLE_ID;
  style1.parent = 0x7f010001;

  resources::StyleResource style2;
  style2.id = MULTI_STYLE_ID;
  style2.parent = 0x7f010002;

  style_info.styles[MULTI_STYLE_ID] = {style1, style2};

  auto parent3 = style_info.get_unambiguous_parent(MULTI_STYLE_ID);
  EXPECT_FALSE(parent3.has_value());

  resources::StyleResource style_no_parent;
  style_no_parent.id = NO_PARENT_ID;

  style_info.styles[NO_PARENT_ID] = {style_no_parent};

  auto parent4 = style_info.get_unambiguous_parent(NO_PARENT_ID);
  EXPECT_TRUE(parent4.has_value());
  EXPECT_EQ(parent4.value(), 0);
}

TEST(RedexResources, StyleInfoGetDepth) {
  const uint32_t NONEXISTENT_ID = 0x7f030001;
  const uint32_t ROOT_ID = 0x7f030002;
  const uint32_t CHILD1_ID = 0x7f030003;
  const uint32_t CHILD2_ID = 0x7f030004;
  const uint32_t GRANDCHILD1_ID = 0x7f030005;
  const uint32_t GRANDCHILD2_ID = 0x7f030006;
  const uint32_t ISOLATED_ID = 0x7f030007;

  auto add_vertex = [](resources::StyleInfo& style_info, uint32_t id) {
    auto vertex =
        boost::add_vertex(resources::StyleInfo::Node{id}, style_info.graph);
    style_info.id_to_vertex[id] = vertex;
    return vertex;
  };

  auto add_edge = [](resources::StyleInfo& style_info,
                     resources::StyleInfo::vertex_t parent,
                     resources::StyleInfo::vertex_t child) {
    boost::add_edge(parent, child, style_info.graph);
  };

  {
    resources::StyleInfo style_info;
    EXPECT_THROW(style_info.get_depth(NONEXISTENT_ID), std::out_of_range);
  }

  {
    resources::StyleInfo style_info;
    add_vertex(style_info, ISOLATED_ID);

    auto depth = style_info.get_depth(ISOLATED_ID);
    EXPECT_EQ(depth, 0u);
  }

  {
    resources::StyleInfo style_info;
    auto root = add_vertex(style_info, ROOT_ID);
    auto child1 = add_vertex(style_info, CHILD1_ID);

    add_edge(style_info, root, child1);

    EXPECT_EQ(style_info.get_depth(ROOT_ID), 1u);
    EXPECT_EQ(style_info.get_depth(CHILD1_ID), 0u);
  }

  // ROOT -> CHILD1 -> GRANDCHILD1 (depth 2)
  //      -> CHILD2 -> GRANDCHILD2 (depth 2)
  {
    resources::StyleInfo style_info;
    auto root = add_vertex(style_info, ROOT_ID);
    auto child1 = add_vertex(style_info, CHILD1_ID);
    auto child2 = add_vertex(style_info, CHILD2_ID);
    auto grandchild1 = add_vertex(style_info, GRANDCHILD1_ID);
    auto grandchild2 = add_vertex(style_info, GRANDCHILD2_ID);

    add_edge(style_info, root, child1);
    add_edge(style_info, root, child2);
    add_edge(style_info, child1, grandchild1);
    add_edge(style_info, child2, grandchild2);

    EXPECT_EQ(style_info.get_depth(ROOT_ID), 2u);
    EXPECT_EQ(style_info.get_depth(CHILD1_ID), 1u);
    EXPECT_EQ(style_info.get_depth(CHILD2_ID), 1u);
    EXPECT_EQ(style_info.get_depth(GRANDCHILD1_ID), 0u);
    EXPECT_EQ(style_info.get_depth(GRANDCHILD2_ID), 0u);
  }

  // Test unbalanced tree
  // ROOT -> CHILD1 -> GRANDCHILD1 (depth 2)
  //      -> CHILD2 (depth 1, child2 is leaf)
  {
    resources::StyleInfo style_info;
    auto root = add_vertex(style_info, ROOT_ID);
    auto child1 = add_vertex(style_info, CHILD1_ID);
    auto child2 = add_vertex(style_info, CHILD2_ID);
    auto grandchild1 = add_vertex(style_info, GRANDCHILD1_ID);

    add_edge(style_info, root, child1);
    add_edge(style_info, root, child2);
    add_edge(style_info, child1, grandchild1);

    EXPECT_EQ(style_info.get_depth(ROOT_ID), 2u);
    EXPECT_EQ(style_info.get_depth(CHILD1_ID), 1u);
    EXPECT_EQ(style_info.get_depth(CHILD2_ID), 0u);
    EXPECT_EQ(style_info.get_depth(GRANDCHILD1_ID), 0u);
  }
}
