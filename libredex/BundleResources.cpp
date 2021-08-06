/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#include <memory>
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

// Traverse a compound value message, and return a list of Item defined in
// this message.
std::vector<aapt::pb::Item> get_items_from_CV(
    const aapt::pb::CompoundValue& comp_value) {
  std::vector<aapt::pb::Item> ret;
  if (comp_value.has_style()) {
    // Style style -> Entry entry -> Item item.
    const auto& entries = comp_value.style().entry();
    for (int n = 0; n < entries.size(); ++n) {
      if (entries[n].has_item()) {
        ret.push_back(entries[n].item());
      }
    }
  } else if (comp_value.has_array()) {
    // Array array -> Element element -> Item item.
    const auto& elements = comp_value.array().element();
    for (int n = 0; n < elements.size(); ++n) {
      if (elements[n].has_item()) {
        ret.push_back(elements[n].item());
      }
    }
  } else if (comp_value.has_plural()) {
    // Plural plural -> Entry entry -> Item item.
    const auto& entries = comp_value.plural().entry();
    for (int n = 0; n < entries.size(); ++n) {
      if (entries[n].has_item()) {
        ret.push_back(entries[n].item());
      }
    }
  }
  return ret;
}

// Traverse a compound value message, and return a list of Reference messages
// used in this message.
std::vector<aapt::pb::Reference> get_references(
    const aapt::pb::CompoundValue& comp_value) {
  std::vector<aapt::pb::Reference> ret;
  // Find refs from Item message.
  const auto& items = get_items_from_CV(comp_value);
  for (size_t i = 0; i < items.size(); i++) {
    if (items[i].has_ref()) {
      ret.push_back(items[i].ref());
    }
  }
  // Find refs from other types of messages.
  if (comp_value.has_attr()) {
    // Attribute attr -> Symbol symbol -> Reference name.
    const auto& symbols = comp_value.attr().symbol();
    for (int i = 0; i < symbols.size(); i++) {
      if (symbols[i].has_name()) {
        ret.push_back(symbols[i].name());
      }
    }
  } else if (comp_value.has_style()) {
    // Style style -> Entry entry -> Reference key.
    const auto& entries = comp_value.style().entry();
    for (int i = 0; i < entries.size(); i++) {
      if (entries[i].has_key()) {
        ret.push_back(entries[i].key());
      }
    }
    // Style style -> Reference parent.
    if (comp_value.style().has_parent()) {
      ret.push_back(comp_value.style().parent());
    }
  } else if (comp_value.has_styleable()) {
    // Styleable styleable -> Entry entry -> Reference attr.
    const auto& entries = comp_value.styleable().entry();
    for (int i = 0; i < entries.size(); i++) {
      if (entries[i].has_attr()) {
        ret.push_back(entries[i].attr());
      }
    }
  }
  return ret;
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

        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          manifest.c_str());
        if (pb_node.has_element() && pb_node.element().name() == "manifest") {
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
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          base_manifest.c_str());
        if (pb_node.has_element()) {
          const auto& manifest_element = pb_node.element();
          for (const aapt::pb::XmlNode& pb_child : manifest_element.child()) {
            if (pb_child.node_case() == aapt::pb::XmlNode::NodeCase::kElement) {
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

namespace {
void apply_rename_map(const std::map<std::string, std::string>& rename_map,
                      aapt::pb::XmlNode* node,
                      size_t* out_num_renamed) {
  // NOTE: The implementation that follows is not at all similar to
  // ApkResources though this is likely sufficient. ApkResources, when
  // renaming will simply iterate through a string pool, picking up anything
  // wherever it might be in the document. This is simply checking tag
  // names, attribute values and text.
  if (node->has_element()) {
    auto element = node->mutable_element();
    {
      auto search = rename_map.find(element->name());
      if (search != rename_map.end()) {
        element->set_name(search->second);
        (*out_num_renamed)++;
      }
    }
    auto attr_size = element->attribute_size();
    for (int i = 0; i < attr_size; i++) {
      auto pb_attr = element->mutable_attribute(i);
      auto search = rename_map.find(pb_attr->value());
      if (search != rename_map.end()) {
        pb_attr->set_value(search->second);
        (*out_num_renamed)++;
      }
    }
    auto child_size = element->child_size();
    for (int i = 0; i < child_size; i++) {
      auto child = element->mutable_child(i);
      apply_rename_map(rename_map, child, out_num_renamed);
    }
  } else {
    auto search = rename_map.find(node->text());
    if (search != rename_map.end()) {
      node->set_text(search->second);
      (*out_num_renamed)++;
    }
  }
}
} // namespace

bool BundleResources::rename_classes_in_layout(
    const std::string& file_path,
    const std::map<std::string, std::string>& rename_map,
    size_t* out_num_renamed) {
  bool write_failed = false;
  read_protobuf_file_contents(
      file_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          file_path.c_str());
        size_t num_renamed = 0;
        apply_rename_map(rename_map, &pb_node, &num_renamed);
        if (num_renamed > 0) {
          std::ofstream out(file_path, std::ofstream::binary);
          if (pb_node.SerializeToOstream(&out)) {
            *out_num_renamed = num_renamed;
          } else {
            write_failed = true;
          }
        }
      });
  return !write_failed;
}

namespace {

std::vector<std::string> find_subdir_in_modules(
    const std::string& extracted_dir, const std::string& subdir) {
  std::vector<std::string> dirs;
  boost::filesystem::path dir(extracted_dir);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto maybe = entry.path() / subdir;
    if (boost::filesystem::exists(maybe)) {
      dirs.emplace_back(maybe.string());
    }
  }
  return dirs;
}

} // namespace

std::vector<std::string> BundleResources::find_res_directories() {
  return find_subdir_in_modules(m_directory, "res");
}

std::vector<std::string> BundleResources::find_lib_directories() {
  return find_subdir_in_modules(m_directory, "lib");
}

std::string BundleResources::get_base_assets_dir() {
  return m_directory + "/base/assets";
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

// Collect all resource ids referred in an given xml element.
// attr->compiled_item->ref->id
void collect_rids_for_element(const aapt::pb::XmlElement& element,
                              std::unordered_set<uint32_t>& result) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (!pb_attr.has_compiled_item()) {
      continue;
    }
    const auto& pb_item = pb_attr.compiled_item();
    if (pb_item.has_ref()) {
      auto rid = pb_item.ref().id();
      if (rid > PACKAGE_RESID_START) {
        result.emplace(rid);
      }
    }
  }
}

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
    // Consider the element name itself as a possible class in the
    // application
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

void change_resource_id_in_pb_reference(
    const std::map<uint32_t, uint32_t>& old_to_new, aapt::pb::Reference* ref) {
  auto ref_id = ref->id();
  if (old_to_new.count(ref_id)) {
    auto new_id = old_to_new.at(ref_id);
    ref->set_id(new_id);
  }
}

void change_resource_id_in_value_reference(
    const std::map<uint32_t, uint32_t>& old_to_new, aapt::pb::Value* value) {

  if (value->has_item()) {
    auto pb_item = value->mutable_item();
    if (pb_item->has_ref()) {
      change_resource_id_in_pb_reference(old_to_new, pb_item->mutable_ref());
    }
  } else if (value->has_compound_value()) {
    auto pb_compound_value = value->mutable_compound_value();
    if (pb_compound_value->has_attr()) {
      auto pb_attr = pb_compound_value->mutable_attr();
      auto symbol_size = pb_attr->symbol_size();
      for (int i = 0; i < symbol_size; ++i) {
        auto symbol = pb_attr->mutable_symbol(i);
        if (symbol->has_name()) {
          change_resource_id_in_pb_reference(old_to_new,
                                             symbol->mutable_name());
        }
      }
    } else if (pb_compound_value->has_style()) {
      auto pb_style = pb_compound_value->mutable_style();
      if (pb_style->has_parent()) {
        change_resource_id_in_pb_reference(old_to_new,
                                           pb_style->mutable_parent());
      }
      auto entry_size = pb_style->entry_size();
      for (int i = 0; i < entry_size; ++i) {
        auto entry = pb_style->mutable_entry(i);
        if (entry->has_key()) {
          change_resource_id_in_pb_reference(old_to_new, entry->mutable_key());
        }
        if (entry->has_item()) {
          auto pb_item = entry->mutable_item();
          if (pb_item->has_ref()) {
            change_resource_id_in_pb_reference(old_to_new,
                                               pb_item->mutable_ref());
          }
        }
      }
    } else if (pb_compound_value->has_styleable()) {
      auto pb_styleable = pb_compound_value->mutable_styleable();
      auto entry_size = pb_styleable->entry_size();
      for (int i = 0; i < entry_size; ++i) {
        auto entry = pb_styleable->mutable_entry(i);
        if (entry->has_attr()) {
          change_resource_id_in_pb_reference(old_to_new, entry->mutable_attr());
        }
      }
    } else if (pb_compound_value->has_array()) {
      auto pb_array = pb_compound_value->mutable_array();
      auto entry_size = pb_array->element_size();
      for (int i = 0; i < entry_size; ++i) {
        auto element = pb_array->mutable_element(i);
        if (element->has_item()) {
          auto pb_item = element->mutable_item();
          if (pb_item->has_ref()) {
            change_resource_id_in_pb_reference(old_to_new,
                                               pb_item->mutable_ref());
          }
        }
      }
    } else if (pb_compound_value->has_plural()) {
      auto pb_plural = pb_compound_value->mutable_plural();
      auto entry_size = pb_plural->entry_size();
      for (int i = 0; i < entry_size; ++i) {
        auto entry = pb_plural->mutable_entry(i);
        if (entry->has_item()) {
          auto pb_item = entry->mutable_item();
          if (pb_item->has_ref()) {
            change_resource_id_in_pb_reference(old_to_new,
                                               pb_item->mutable_ref());
          }
        }
      }
    }
  }
}

void remove_or_change_resource_ids(
    const std::set<uint32_t>& ids_to_remove,
    const std::map<uint32_t, uint32_t>& old_to_new,
    uint32_t package_id,
    aapt::pb::Type* type) {
  google::protobuf::RepeatedPtrField<aapt::pb::Entry> new_entries;
  for (const auto& entry : type->entry()) {
    uint32_t res_id =
        (PACKAGE_MASK_BIT & (package_id << PACKAGE_INDEX_BIT_SHIFT)) |
        (TYPE_MASK_BIT & ((type->type_id().id()) << TYPE_INDEX_BIT_SHIFT)) |
        (ENTRY_MASK_BIT & (entry.entry_id().id()));
    if (ids_to_remove.count(res_id)) {
      continue;
    }
    auto copy_entry = new aapt::pb::Entry(entry);
    if (old_to_new.count(res_id)) {
      uint32_t new_res_id = old_to_new.at(res_id);
      uint32_t new_entry_id = ENTRY_MASK_BIT & new_res_id;
      always_assert_log(copy_entry->has_entry_id(),
                        "Entry don't have id %s",
                        copy_entry->DebugString().c_str());
      auto entry_id = copy_entry->mutable_entry_id();
      entry_id->set_id(new_entry_id);
      auto config_value_size = copy_entry->config_value_size();
      for (int i = 0; i < config_value_size; ++i) {
        auto config_value = copy_entry->mutable_config_value(i);
        always_assert_log(config_value->has_value(),
                          "ConfigValue don't have value %s\nEntry:\n%s",
                          config_value->DebugString().c_str(),
                          copy_entry->DebugString().c_str());
        auto value = config_value->mutable_value();
        change_resource_id_in_value_reference(old_to_new, value);
      }
    }
    new_entries.AddAllocated(copy_entry);
  }
  type->clear_entry();
  type->mutable_entry()->Swap(&new_entries);
}

void change_resource_id_in_xml_references(
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids,
    aapt::pb::XmlNode* node,
    size_t* num_resource_id_changed) {
  if (!node->has_element()) {
    return;
  }
  auto element = node->mutable_element();
  auto attr_size = element->attribute_size();
  for (int i = 0; i < attr_size; i++) {
    auto pb_attr = element->mutable_attribute(i);
    if (pb_attr->has_compiled_item()) {
      auto pb_item = pb_attr->mutable_compiled_item();
      if (pb_item->has_ref()) {
        auto ref = pb_item->mutable_ref();
        auto ref_id = ref->id();
        if (kept_to_remapped_ids.count(ref_id)) {
          auto new_id = kept_to_remapped_ids.at(ref_id);
          (*num_resource_id_changed)++;
          ref->set_id(new_id);
        }
      }
    }
  }
  auto child_size = element->child_size();
  for (int i = 0; i < child_size; i++) {
    auto child = element->mutable_child(i);
    change_resource_id_in_xml_references(kept_to_remapped_ids, child,
                                         num_resource_id_changed);
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
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          file_path.c_str());
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
                    element, ns_uri_to_prefix, attributes_to_read, out_classes,
                    out_attributes);
                return true;
              });
        }
      });
}

size_t BundleResources::remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) {
  if (is_raw_resource(filename)) {
    return 0;
  }
  TRACE(RES,
        9,
        "BundleResources changing resource id for xml file: %s",
        filename.c_str());
  size_t num_changed = 0;
  read_protobuf_file_contents(
      filename,
      [&](google::protobuf::io::CodedInputStream& input, size_t /* unused */) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          filename.c_str());
        change_resource_id_in_xml_references(kept_to_remapped_ids, &pb_node,
                                             &num_changed);
        if (num_changed > 0) {
          std::ofstream out(filename, std::ofstream::binary);
          always_assert(pb_node.SerializeToOstream(&out));
        }
      });
  return num_changed;
}

std::vector<std::string> BundleResources::find_resources_files() {
  std::vector<std::string> paths;
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto resources_file = entry.path() / "resources.pb";
    if (boost::filesystem::exists(resources_file)) {
      paths.emplace_back(resources_file.string());
    }
  }
  return paths;
}

std::unordered_set<std::string> BundleResources::find_all_xml_files() {
  std::unordered_set<std::string> all_xml_files;
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto manifest = entry.path() / "manifest/AndroidManifest.xml";
    if (boost::filesystem::exists(manifest)) {
      all_xml_files.emplace(manifest.string());
    }
    auto res_path = entry.path() / "/res";
    for (const std::string& path : get_xml_files(res_path.string())) {
      all_xml_files.emplace(path);
    }
  }
  return all_xml_files;
}

std::unordered_set<uint32_t> BundleResources::get_xml_reference_attributes(
    const std::string& filename) {
  std::unordered_set<uint32_t> result;
  if (is_raw_resource(filename)) {
    return result;
  }

  read_protobuf_file_contents(
      filename,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResource failed to read %s",
                          filename.c_str());
        if (pb_node.has_element()) {
          const auto& start = pb_node.element();
          traverse_element_and_children(
              start, [&](const aapt::pb::XmlElement& element) {
                collect_rids_for_element(element, result);
                return true;
              });
        }
      });
  return result;
}

void ResourcesPbFile::remap_res_ids_and_serialize(
    const std::vector<std::string>& resource_files,
    const std::map<uint32_t, uint32_t>& old_to_new) {
  for (const auto& resources_pb_path : resource_files) {
    TRACE(RES,
          9,
          "BundleResources changing resource data for file: %s",
          resources_pb_path.c_str());
    read_protobuf_file_contents(
        resources_pb_path,
        [&](google::protobuf::io::CodedInputStream& input,
            size_t /* unused */) {
          aapt::pb::ResourceTable pb_restable;
          bool read_finish = pb_restable.ParseFromCodedStream(&input);
          always_assert_log(read_finish,
                            "BundleResoource failed to read %s",
                            resources_pb_path.c_str());
          int package_size = pb_restable.package_size();
          for (int i = 0; i < package_size; i++) {
            auto package = pb_restable.mutable_package(i);
            auto current_package_id = package->package_id().id();
            int type_size = package->type_size();
            for (int j = 0; j < type_size; j++) {
              auto type = package->mutable_type(j);
              remove_or_change_resource_ids(m_ids_to_remove, old_to_new,
                                            current_package_id, type);
            }
          }
          std::ofstream out(resources_pb_path, std::ofstream::binary);
          always_assert(pb_restable.SerializeToOstream(&out));
        });
  }
}

void ResourcesPbFile::collect_resource_data_for_file(
    const std::string& resources_pb_path) {
  TRACE(RES,
        9,
        "BundleResources collecting resource data for file: %s",
        resources_pb_path.c_str());
  read_protobuf_file_contents(
      resources_pb_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t /* unused */) {
        aapt::pb::ResourceTable pb_restable;
        bool read_finish = pb_restable.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          resources_pb_path.c_str());
        for (const aapt::pb::Package& pb_package : pb_restable.package()) {
          auto current_package_id = pb_package.package_id().id();
          TRACE(RES, 9, "Package: %s %X", pb_package.package_name().c_str(),
                current_package_id);
          for (const aapt::pb::Type& pb_type : pb_package.type()) {
            auto current_type_id = pb_type.type_id().id();
            const auto& current_type_name = pb_type.name();
            TRACE(RES, 9, "  Type: %s %X", current_type_name.c_str(),
                  current_type_id);
            always_assert(m_type_id_to_names.count(current_type_id) == 0 ||
                          m_type_id_to_names.at(current_type_id) ==
                              current_type_name);
            m_type_id_to_names[current_type_id] = current_type_name;
            for (const aapt::pb::Entry& pb_entry : pb_type.entry()) {
              if (m_package_id == 0xFFFFFFFF) {
                m_package_id = current_package_id;
              }
              always_assert_log(
                  m_package_id == current_package_id,
                  "Broken assumption for only one package for resources.");
              std::string name_string = pb_entry.name();
              auto current_entry_id = pb_entry.entry_id().id();
              auto current_resource_id =
                  (PACKAGE_MASK_BIT &
                   (current_package_id << PACKAGE_INDEX_BIT_SHIFT)) |
                  (TYPE_MASK_BIT & (current_type_id << TYPE_INDEX_BIT_SHIFT)) |
                  (ENTRY_MASK_BIT & current_entry_id);
              TRACE(RES, 9, "    Entry: %s %X %X", pb_entry.name().c_str(),
                    current_entry_id, current_resource_id);
              sorted_res_ids.add(current_resource_id);
              always_assert(m_existed_res_ids.count(current_resource_id) == 0);
              m_existed_res_ids.emplace(current_resource_id);
              id_to_name.emplace(current_resource_id, name_string);
              name_to_ids[name_string].push_back(current_resource_id);
              m_res_id_to_configvalue.emplace(current_resource_id,
                                              pb_entry.config_value());
            }
          }
        }
      });
}

std::unordered_set<uint32_t> ResourcesPbFile::get_types_by_name(
    const std::unordered_set<std::string>& type_names) {
  always_assert(m_type_id_to_names.size() > 0);
  std::unordered_set<uint32_t> type_ids;
  for (const auto& pair : m_type_id_to_names) {
    if (type_names.count(pair.second) == 1) {
      type_ids.emplace((pair.first) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

void ResourcesPbFile::delete_resource(uint32_t res_id) {
  // Keep track of res_id and delete later in remap_res_ids_and_serialize.
  m_ids_to_remove.emplace(res_id);
}

std::unordered_set<std::string> ResourcesPbFile::get_files_by_rid(
    uint32_t res_id) {
  std::unordered_set<std::string> ret;
  if (m_res_id_to_configvalue.count(res_id) == 0) {
    return ret;
  }
  const auto& out_values = m_res_id_to_configvalue.at(res_id);
  for (auto i = 0; i < out_values.size(); i++) {
    const auto& value = out_values[i].value();
    if (value.has_item() && value.item().has_file()) {
      // Item
      auto file_path = value.item().file().path();
      if (is_resource_file(file_path)) {
        ret.emplace(file_path);
      }
    } else if (value.has_compound_value()) {
      // For coumpound value, we flatten it and check all its item messages.
      const auto& items = get_items_from_CV(value.compound_value());
      for (size_t n = 0; n < items.size(); n++) {
        if (items[n].has_file()) {
          auto file_path = items[n].file().path();
          if (is_resource_file(file_path)) {
            ret.emplace(file_path);
          }
        }
      }
    }
  }
  return ret;
}

void ResourcesPbFile::walk_references_for_resource(
    uint32_t resID,
    std::unordered_set<uint32_t>* nodes_visited,
    std::unordered_set<std::string>* leaf_string_values) {
  if (nodes_visited->find(resID) != nodes_visited->end()) {
    // Return directly if a node is visited.
    return;
  }
  nodes_visited->emplace(resID);

  const auto& initial_values = get_res_id_to_configvalue().at(resID);

  std::stack<aapt::pb::ConfigValue> nodes_to_explore;
  for (int index = 0; index < initial_values.size(); ++index) {
    nodes_to_explore.push(initial_values[index]);
  }

  while (!nodes_to_explore.empty()) {
    const auto& r = nodes_to_explore.top();
    nodes_to_explore.pop();

    const auto& value = r.value();

    std::vector<aapt::pb::Item> items;
    std::vector<aapt::pb::Reference> refs;

    if (value.has_compound_value()) {
      items = get_items_from_CV(value.compound_value());
      refs = get_references(value.compound_value());
    } else {
      items.push_back(value.item());
      if (value.item().has_ref()) {
        refs.push_back(value.item().ref());
      }
    }

    // For each Item, store the path of FileReference into string values.
    for (size_t i = 0; i < items.size(); i++) {
      const auto& item = items[i];
      if (item.has_file()) {
        leaf_string_values->insert(item.file().path());
        continue;
      }
    }

    // For each Reference, follow its id to traverse the resources.
    for (size_t i = 0; i < refs.size(); i++) {
      std::vector<uint32_t> ref_ids;
      if (refs[i].id() != 0) {
        ref_ids.push_back(refs[i].id());
      } else if (!refs[i].name().empty()) {
        // Since id of a Reference message is optional, once ref_id =0, it is
        // possible that the resource is refered by name. If we can make sure it
        // won't happen, this branch can be removed.
        ref_ids = get_res_ids_by_name(refs[i].name());
      }

      for (size_t n = 0; n < ref_ids.size(); n++) {
        // Skip if the node has been visited.
        const auto ref_id = ref_ids[n];
        if (ref_id <= PACKAGE_RESID_START ||
            nodes_visited->find(ref_id) != nodes_visited->end()) {
          continue;
        }
        nodes_visited->insert(ref_id);
        const auto& inner_values = (get_res_id_to_configvalue()).at(ref_id);
        for (auto index = 0; index < inner_values.size(); ++index) {
          nodes_to_explore.push(inner_values[index]);
        }
      }
    }
  }
}

std::unique_ptr<ResourceTableFile> BundleResources::load_res_table() {
  const auto& res_pb_file_paths = find_resources_files();
  auto to_return = std::make_unique<ResourcesPbFile>(ResourcesPbFile());
  for (const auto& res_pb_file_path : res_pb_file_paths) {
    to_return->collect_resource_data_for_file(res_pb_file_path);
  }
  return to_return;
}

BundleResources::~BundleResources() {}

size_t ResourcesPbFile::get_hash_from_values(
    const ConfigValues& config_values) {
  size_t hash = 0;
  for (int i = 0; i < config_values.size(); ++i) {
    const auto& value = config_values[i].value();
    std::string value_str;
    if (value.has_item()) {
      value.item().SerializeToString(&value_str);
    } else {
      value.compound_value().SerializeToString(&value_str);
    }
    boost::hash_combine(hash, value_str);
  }
  return hash;
}

void ResourcesPbFile::collect_resid_values_and_hashes(
    const std::vector<uint32_t>& ids,
    std::map<size_t, std::vector<uint32_t>>* res_by_hash) {
  for (uint32_t id : ids) {
    const auto& config_values = m_res_id_to_configvalue.at(id);
    (*res_by_hash)[get_hash_from_values(config_values)].push_back(id);
  }
}

bool ResourcesPbFile::resource_value_identical(uint32_t a_id, uint32_t b_id) {
  if ((a_id & PACKAGE_MASK_BIT) != (b_id & PACKAGE_MASK_BIT) ||
      (a_id & TYPE_MASK_BIT) != (b_id & TYPE_MASK_BIT)) {
    return false;
  }
  const auto& config_values_a = m_res_id_to_configvalue.at(a_id);
  const auto& config_values_b = m_res_id_to_configvalue.at(b_id);
  if (config_values_a.size() != config_values_b.size()) {
    return false;
  }
  // For ResTable in arsc there seems to be assumption that configuration
  // will be in same order for list of configvalues.
  // https://fburl.com/code/optgs5k3 Not sure if this will hold for protobuf
  // representation as well.
  for (int i = 0; i < config_values_a.size(); ++i) {
    const auto& config_value_a = config_values_a[i];
    const auto& config_value_b = config_values_b[i];

    const auto& config_a = config_value_a.config();
    std::string config_a_str;
    config_a.SerializeToString(&config_a_str);
    const auto& config_b = config_value_b.config();
    std::string config_b_str;
    config_b.SerializeToString(&config_b_str);
    if (config_a_str != config_b_str) {
      return false;
    }

    const auto& value_a = config_value_a.value();
    const auto& value_b = config_value_b.value();
    // Not sure if this should be compared
    if (value_a.weak() != value_b.weak()) {
      return false;
    }
    if (value_a.has_item() != value_b.has_item()) {
      return false;
    }
    std::string value_a_str;
    std::string value_b_str;
    if (value_a.has_item()) {
      value_a.item().SerializeToString(&value_a_str);
      value_b.item().SerializeToString(&value_b_str);
    } else {
      value_a.compound_value().SerializeToString(&value_a_str);
      value_b.compound_value().SerializeToString(&value_b_str);
    }
    if (value_a_str != value_b_str) {
      return false;
    }
  }
  return true;
}

ResourcesPbFile::~ResourcesPbFile() {}

#endif // HAS_PROTOBUF
