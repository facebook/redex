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
#include <boost/range/iterator_range.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>

#include <google/protobuf/io/coded_stream.h>

#include "Debug.h"
#include "DexUtil.h"
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

bool has_attribute(const aapt::pb::XmlElement& element,
                   const std::string& name) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      return true;
    }
  }
  return false;
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

bool get_bool_attribute_value(const aapt::pb::XmlElement& element,
                              const std::string& name,
                              bool default_value) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      if (pb_attr.has_compiled_item()) {
        const auto& pb_item = pb_attr.compiled_item();
        if (pb_item.has_prim() && pb_item.prim().oneof_value_case() ==
                                      aapt::pb::Primitive::kBooleanValue) {
          return pb_item.prim().boolean_value();
        }
      }
      return default_value;
    }
  }
  return default_value;
}

std::string get_string_attribute_value(const aapt::pb::XmlElement& element,
                                       const std::string& name) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      always_assert_log(!pb_attr.has_compiled_item(),
                        "Attribute %s expected to be a string!",
                        name.c_str());
      return pb_attr.value();
    }
  }
  return std::string("");
}

// Apply callback to element and its descendants, stopping if/when callback
// returns false
void traverse_element_and_children(
    const aapt::pb::XmlElement& start,
    const std::function<bool(const aapt::pb::XmlElement&)>& callback) {
  std::queue<aapt::pb::XmlElement> q;
  q.push(start);
  while (!q.empty()) {
    const auto& front = q.front();
    if (!callback(front)) {
      return;
    }
    for (const aapt::pb::XmlNode& pb_child : front.child()) {
      if (pb_child.node_case() == aapt::pb::XmlNode::NodeCase::kElement) {
        q.push(pb_child.element());
      }
    }
    q.pop();
  }
}

// Look for <search_tag> within the descendants of the given XML Node
bool find_nested_tag(const std::string& search_tag,
                     const aapt::pb::XmlElement& start) {
  bool find_result = false;
  traverse_element_and_children(
      start, [&](const aapt::pb::XmlElement& element) {
        bool keep_going = true;
        if (&start != &element && element.name() == search_tag) {
          find_result = true;
          keep_going = false;
        }
        return keep_going;
      });
  return find_result;
}

inline std::string fully_qualified_external(const std::string& package_name,
                                            const std::string& value) {
  if (value.empty()) {
    return value;
  }
  if (value.at(0) == '.') {
    return java_names::external_to_internal(package_name + value);
  }
  return java_names::external_to_internal(value);
}

void read_single_manifest(const std::string& manifest,
                          ManifestClassInfo* manifest_classes) {
  TRACE(RES, 1, "Reading proto manifest at %s", manifest.c_str());
  read_protobuf_file_contents(
      manifest,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        std::unordered_map<std::string, ComponentTag> string_to_tag{
            {"activity", ComponentTag::Activity},
            {"activity-alias", ComponentTag::ActivityAlias},
            {"provider", ComponentTag::Provider},
            {"receiver", ComponentTag::Receiver},
            {"service", ComponentTag::Service},
        };
        aapt::pb::XmlNode pb_node;
        if (pb_node.ParseFromCodedStream(&input) && pb_node.has_element() &&
            pb_node.element().name() == "manifest") {
          const auto& manifest_element = pb_node.element();
          auto package_name =
              get_string_attribute_value(manifest_element, "package");
          traverse_element_and_children(
              manifest_element, [&](const aapt::pb::XmlElement& element) {
                const auto& tag = element.name();
                if (tag == "application") {
                  auto classname = get_string_attribute_value(element, "name");
                  if (!classname.empty()) {
                    manifest_classes->application_classes.emplace(
                        fully_qualified_external(package_name, classname));
                  }
                  auto app_factory_cls = get_string_attribute_value(
                      element, "appComponentFactory");
                  if (!app_factory_cls.empty()) {
                    manifest_classes->application_classes.emplace(
                        fully_qualified_external(package_name,
                                                 app_factory_cls));
                  }
                } else if (tag == "instrumentation") {
                  auto classname = get_string_attribute_value(element, "name");
                  always_assert(classname.size());
                  manifest_classes->instrumentation_classes.emplace(
                      fully_qualified_external(package_name, classname));
                } else if (string_to_tag.count(tag)) {
                  std::string classname = get_string_attribute_value(
                      element,
                      tag != "activity-alias" ? "name" : "targetActivity");
                  always_assert(classname.size());

                  bool has_exported_attribute = has_primitive_attribute(
                      element, "exported", aapt::pb::Primitive::kBooleanValue);
                  bool has_permission_attribute =
                      has_attribute(element, "permission");
                  bool has_protection_level_attribute =
                      has_attribute(element, "protectionLevel");
                  bool is_exported =
                      get_bool_attribute_value(element, "exported",
                                               /* default_value */ false);

                  BooleanXMLAttribute export_attribute;
                  if (has_exported_attribute) {
                    if (is_exported) {
                      export_attribute = BooleanXMLAttribute::True;
                    } else {
                      export_attribute = BooleanXMLAttribute::False;
                    }
                  } else {
                    export_attribute = BooleanXMLAttribute::Undefined;
                  }
                  // NOTE: This logic is analogous to the APK manifest reading
                  // code, which is wrong. This should be a bitmask, not a
                  // string. Returning the same messed up values here to at
                  // least be consistent for now.
                  std::string permission_attribute;
                  std::string protection_level_attribute;
                  if (has_permission_attribute) {
                    permission_attribute =
                        get_string_attribute_value(element, "permission");
                  }
                  if (has_protection_level_attribute) {
                    protection_level_attribute =
                        get_string_attribute_value(element, "protectionLevel");
                  }

                  ComponentTagInfo tag_info(
                      string_to_tag.at(tag),
                      fully_qualified_external(package_name, classname),
                      export_attribute,
                      permission_attribute,
                      protection_level_attribute);
                  if (tag == "provider") {
                    auto text =
                        get_string_attribute_value(element, "authorities");
                    parse_authorities(text, &tag_info.authority_classes);
                  } else {
                    tag_info.has_intent_filters =
                        find_nested_tag("intent-filter", element);
                  }
                  manifest_classes->component_tags.emplace_back(tag_info);
                }
                return true;
              });
        }
      });
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
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto manifest = entry.path() / "manifest/AndroidManifest.xml";
    if (boost::filesystem::exists(manifest)) {
      read_single_manifest(manifest.string(), &manifest_classes);
    }
  }
  return manifest_classes;
}

void BundleResources::rename_classes_in_layouts(
    const std::map<std::string, std::string>& rename_map) {
  // TODO
}

std::vector<std::string> BundleResources::find_res_directories() {
  std::vector<std::string> dirs;
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto res_dir = entry.path() / "res";
    if (boost::filesystem::exists(res_dir)) {
      dirs.emplace_back(res_dir.string());
    }
  }
  return dirs;
}

namespace {
const std::unordered_set<std::string> NON_CLASS_ELEMENTS = {
    "fragment", "view", "dialog", "activity", "intent",
};
const std::vector<std::string> CLASS_XML_ATTRIBUTES = {
    "class",
    "name",
    "targetClass",
};

void collect_layout_classes_and_attributes_for_element(
    const aapt::pb::XmlElement& element,
    const std::unordered_map<std::string, std::string>& ns_uri_to_prefix,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>* out_classes,
    std::unordered_multimap<std::string, std::string>* out_attributes) {
  const auto& element_name = element.name();
  if (NON_CLASS_ELEMENTS.count(element_name) > 0) {
    for (const auto& attr : CLASS_XML_ATTRIBUTES) {
      auto classname = get_string_attribute_value(element, attr);
      if (!classname.empty() && classname.find('.') != std::string::npos) {
        auto internal = java_names::external_to_internal(classname);
        TRACE(RES, 9,
              "Considering %s as possible class in XML "
              "resource from element %s",
              internal.c_str(), element_name.c_str());
        out_classes->emplace(internal);
        break;
      }
    }
  } else if (element_name.find('.') != std::string::npos) {
    // Consider the element name itself as a possible class in the application
    auto internal = java_names::external_to_internal(element_name);
    TRACE(RES, 9, "Considering %s as possible class in XML resource",
          internal.c_str());
    out_classes->emplace(internal);
  }

  if (!attributes_to_read.empty()) {
    for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
      const auto& attr_name = pb_attr.name();
      const auto& uri = pb_attr.namespace_uri();
      std::string fully_qualified =
          ns_uri_to_prefix.count(uri) == 0
              ? attr_name
              : (ns_uri_to_prefix.at(uri) + ":" + attr_name);
      if (attributes_to_read.count(fully_qualified) > 0) {
        always_assert_log(!pb_attr.has_compiled_item(),
                          "Only supporting string values for attributes. "
                          "Given attribute: %s",
                          fully_qualified.c_str());
        auto value = pb_attr.value();
        out_attributes->emplace(fully_qualified, value);
      }
    }
  }
}
} // namespace

void BundleResources::collect_layout_classes_and_attributes_for_file(
    const std::string& file_path,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>* out_classes,
    std::unordered_multimap<std::string, std::string>* out_attributes) {
  if (is_raw_resource(file_path)) {
    return;
  }
  TRACE(RES,
        9,
        "BundleResources collecting classes and attributes for file: %s",
        file_path.c_str());
  read_protobuf_file_contents(
      file_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        if (pb_node.ParseFromCodedStream(&input)) {
          if (pb_node.has_element()) {
            const auto& root = pb_node.element();
            std::unordered_map<std::string, std::string> ns_uri_to_prefix;
            for (const auto& ns_decl : root.namespace_declaration()) {
              if (!ns_decl.uri().empty() && !ns_decl.prefix().empty()) {
                ns_uri_to_prefix.emplace(ns_decl.uri(), ns_decl.prefix());
              }
            }
            traverse_element_and_children(
                root, [&](const aapt::pb::XmlElement& element) {
                  collect_layout_classes_and_attributes_for_element(
                      element, ns_uri_to_prefix, attributes_to_read,
                      out_classes, out_attributes);
                  return true;
                });
          }
        }
      });
}
#endif // HAS_PROTOBUF
