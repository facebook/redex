/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/message.h>
#include <gtest/gtest.h>
#include <set>

#include "BundleResources.h"
#include "FullyQualifyLayoutsVerifyHelper.h"
#include "ReadMaybeMapped.h"
#include "RedexMappedFile.h"
#include "protores/Resources.pb.h"
#include "verify/VerifyUtil.h"

namespace {
void read_element_and_class_attr(const aapt::pb::XmlNode& pb_node,
                                 std::vector<Element>* results) {
  if (!pb_node.has_element()) {
    return;
  }
  const auto& pb_element = pb_node.element();
  Element element{};
  element.name = pb_element.name();
  for (const aapt::pb::XmlAttribute& pb_attr : pb_element.attribute()) {
    if ("class" == pb_attr.name()) {
      element.string_attributes.emplace(pb_attr.name(), pb_attr.value());
    }
  }
  results->emplace_back(element);
  for (const aapt::pb::XmlNode& pb_child : pb_element.child()) {
    read_element_and_class_attr(pb_child, results);
  }
}
} // namespace

TEST_F(PostVerify, BundleFullyQualifyLayoutsTest) {
  auto file_path = resources.at("base/res/layout/test_views.xml");
  std::vector<Element> elements;
  // Parse the nodes, flatten them to a vector and just capture only the
  // attributes/values we really care about for validation purposes.
  redex::read_file_with_contents(file_path, [&](const char* data, size_t size) {
    if (size == 0) {
      fprintf(stderr, "Unable to read protobuf file: %s\n", file_path.c_str());
      return;
    }
    google::protobuf::io::CodedInputStream input(
        (const google::protobuf::uint8*)data, size);
    aapt::pb::XmlNode pb_node;
    always_assert_log(pb_node.ParseFromCodedStream(&input),
                      "Failed to read %s",
                      file_path.c_str());
    read_element_and_class_attr(pb_node, &elements);
  });
  verify_xml_element_attributes(elements);
}
