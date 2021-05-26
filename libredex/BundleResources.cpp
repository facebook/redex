/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#ifdef HAS_PROTOBUF
#include "BundleResources.h"

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <fstream>
#include <iostream>
#include <string>

#include "ReadMaybeMapped.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "Trace.h"

#include "protores/Resources.pb.h"

namespace {

void read_protobuf_file_contents(
    const std::string& file,
    const std::function<void(google::protobuf::io::CodedInputStream&, size_t)>&
        fn) {
  redex::read_file_with_contents(file, [&](const char* data, size_t size) {
    if (size == 0) {
      fprintf(stderr, "Unable to read protobuf file: %s\n", file.c_str());
      return;
    }
    google::protobuf::io::CodedInputStream input(
        (const google::protobuf::uint8*)data, size);
    fn(input, size);
  });
}

bool has_primitive_attribute(const aapt::pb::XmlElement& element,
                             const std::string& name,
                             const aapt::pb::Primitive::OneofValueCase type) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      if (pb_attr.has_compiled_item()) {
        const auto& pb_item = pb_attr.compiled_item();
        if (pb_item.has_prim() && pb_item.prim().oneof_value_case() == type) {
          return true;
        }
      }
      return false;
    }
  }
  return false;
}

int get_int_attribute_value(const aapt::pb::XmlElement& element,
                            const std::string& name) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      if (pb_attr.has_compiled_item()) {
        const auto& pb_item = pb_attr.compiled_item();
        if (pb_item.has_prim() && pb_item.prim().oneof_value_case() ==
                                      aapt::pb::Primitive::kIntDecimalValue) {
          return pb_item.prim().int_decimal_value();
        }
      }
    }
  }
  throw std::runtime_error("Expected element " + element.name() +
                           " to have an int attribute " + name);
}
} // namespace

boost::optional<int32_t> BundleResources::get_min_sdk() {
  std::string base_manifest = (boost::filesystem::path(m_directory) /
                               "base/manifest/AndroidManifest.xml")
                                  .string();
  boost::optional<int32_t> result = boost::none;
  if (!boost::filesystem::exists(base_manifest)) {
    return result;
  }
  TRACE(RES, 1, "Reading proto xml at %s", base_manifest.c_str());
  read_protobuf_file_contents(
      base_manifest,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        if (pb_node.ParseFromCodedStream(&input)) {
          if (pb_node.has_element()) {
            const auto& manifest_element = pb_node.element();
            for (const aapt::pb::XmlNode& pb_child : manifest_element.child()) {
              if (pb_child.node_case() ==
                  aapt::pb::XmlNode::NodeCase::kElement) {
                const auto& pb_element = pb_child.element();
                if (pb_element.name() == "uses-sdk") {
                  if (has_primitive_attribute(
                          pb_element,
                          "minSdkVersion",
                          aapt::pb::Primitive::kIntDecimalValue)) {
                    result = boost::optional<int32_t>(
                        get_int_attribute_value(pb_element, "minSdkVersion"));
                    return;
                  }
                }
              }
            }
          }
        }
      });
  return result;
}

ManifestClassInfo BundleResources::get_manifest_class_info() {
  ManifestClassInfo manifest_classes;
  // TODO
  return manifest_classes;
}
#endif // HAS_PROTOBUF
