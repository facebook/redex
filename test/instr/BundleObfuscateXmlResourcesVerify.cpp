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
#include "ObfuscateXmlVerifyHelper.h"
#include "ReadMaybeMapped.h"
#include "RedexMappedFile.h"
#include "protores/Resources.pb.h"
#include "verify/VerifyUtil.h"

namespace {

void read_attributes(const aapt::pb::XmlNode& pb_node,
                     std::set<std::string>* results) {
  if (!pb_node.has_element()) {
    return;
  }
  const auto& pb_element = pb_node.element();
  for (const aapt::pb::XmlAttribute& pb_attr : pb_element.attribute()) {
    results->emplace(pb_attr.name());
  }
  for (const aapt::pb::XmlNode& pb_child : pb_element.child()) {
    read_attributes(pb_child, results);
  }
}

std::set<std::string> collect_all_attributes(const std::string& file_path) {
  std::set<std::string> results;
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
    read_attributes(pb_node, &results);
  });
  return results;
}
} // namespace

TEST_F(PostVerify, ApkObfuscateXmlResourceTest) {
  std::unordered_map<std::string, std::string> files_without_base;
  // Make common path names so same validation can be run against .apk and .aab
  // files.
  files_without_base["res/layout/activity_main.xml"] =
      resources["base/res/layout/activity_main.xml"];
  files_without_base["res/layout/themed.xml"] =
      resources["base/res/layout/themed.xml"];
  verify_kept_xml_attributes(files_without_base, collect_all_attributes);
}
