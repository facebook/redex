/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/iostreams/device/mapped_file.hpp>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <secure_lib/secure_string.h>

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/Vector.h"
#include "utils/Visitor.h"

namespace {
class XmlAttributeSetter : public arsc::SimpleXmlParser {
 public:
  ~XmlAttributeSetter() override {}

  XmlAttributeSetter(const std::string& tag_name,
                     bool is_using_attr_id,
                     const char* attribute_input,
                     uint32_t attribute_id,
                     uint32_t data_input)
      : m_tag_name(tag_name),
        m_is_using_attr_id(is_using_attr_id),
        m_attribute(attribute_input),
        m_attribute_id(attribute_id),
        m_data(data_input) {}

  bool visit_attribute_ids(uint32_t* id, size_t count) override {
    m_ids = id;
    return arsc::SimpleXmlParser::visit_attribute_ids(id, count);
  }

  // give a stringPool ref of a name, return name in string format.
  std::string get_name_string(
      const struct android::ResStringPool_ref& name_ref) {
    auto idx = dtohl(name_ref.index);
    size_t len;
    auto& pool = global_strings();
    auto chars = pool.stringAt(idx, &len);
    if (chars != nullptr) {
      android::String16 s16(chars, len);
      android::String8 s8(s16);
      return std::string(s8.string());
    } else {
      return std::string();
    }
  }

  bool visit_start_tag(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension) override {
    auto current_name = get_name_string(extension->name);
    // you cannot give a empty string as m_tag_name
    m_found_tag = m_tag_name == current_name;
    return SimpleXmlParser::visit_start_tag(node, extension);
  }

  bool visit_attribute(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension,
                       android::ResXMLTree_attribute* attribute) override {
    if (m_found_tag) {
      std::string attr_name = get_name_string(attribute->name);
      auto attr_idx = dtohl(attribute->name.index);
      bool found_attribute = false;
      if (m_is_using_attr_id && attr_idx < attribute_count() &&
          m_ids[attr_idx] == m_attribute_id) {
        std::cout << "Found target attribute 0x" << std::hex << m_ids[attr_idx]
                  << " at file offset 0x" << get_file_offset(attribute)
                  << std::endl;
        found_attribute = true;
      } else if (attr_name == m_attribute) {
        std::cout << "Found target attribute " << m_attribute
                  << " at file offset 0x" << get_file_offset(attribute)
                  << std::endl;
        found_attribute = true;
      }
      if (found_attribute) {
        attribute->typedValue.data = m_data;
        if (attribute->typedValue.dataType == android::Res_value::TYPE_STRING) {
          attribute->rawValue.index = m_data;
        }
      }
    }
    return arsc::SimpleXmlParser::visit_attribute(node, extension, attribute);
  }

  // Item to find and data to set
  std::string m_tag_name;
  // Mode
  bool m_is_using_attr_id = false;
  // attribute to search, could be id or name
  const char* m_attribute;
  uint32_t m_attribute_id = 0;
  uint32_t m_data;
  uint32_t* m_ids;
  // State
  bool m_found_tag = false;
};
} // namespace

// Writes the new string into the file's pool, if necessary and returns the pool
// index (or a negative value on error).
ssize_t ensure_string_in_xml_string_pool(const std::string& path,
                                         const std::string& new_string) {
  android::Vector<char> new_bytes;
  size_t idx{0};
  {
    auto map = std::make_unique<boost::iostreams::mapped_file>();
    auto mode = (std::ios_base::openmode)std::ios_base::in;
    map->open(path, mode);
    if (!map->is_open()) {
      std::cerr << "Could not map " << path << std::endl;
      return -1;
    }

    auto result = arsc::ensure_string_in_xml_pool(
        map->const_data(), map->size(), new_string, &new_bytes, &idx);
    if (result != android::OK) {
      std::cerr << "Unable to parse " << path << std::endl;
      return -1;
    }
  }
  if (!new_bytes.empty()) {
    std::ofstream ofs(path,
                      std::ofstream::out | std::ofstream::trunc |
                          std::ofstream::binary);
    ofs.write(&(new_bytes[0]), new_bytes.size());
  }
  return idx;
}

// This tool accepts a tag name, attribute ID as defined in the Android SDK, see
// https://cs.android.com/android/platform/superproject/+/android-12.0.0_r1:prebuilts/sdk/3/public/api/android.txt;l=344
// and the raw bytes to set for the attribute's value.
int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "Usage: xml_editor AndroidManifest.xml <tag_name> <attribute "
                 "id/name> "
                 "<new attribute value in string/hex num>\n";
    return 1;
  }
  auto path = std::string(argv[1]);
  auto tag_name = std::string(argv[2]);
  bool is_using_attr_id = false;
  uint32_t attribute_id = 0;
  if (checked_strncmp(argv[3], strlen(argv[3]), "0x", 2, 2) == 0) {
    is_using_attr_id = true;
    attribute_id = (uint32_t)(strtol(argv[3], nullptr, 0));
  }
  uint32_t data;
  if (checked_strncmp(argv[4], strlen(argv[4]), "0x", 2, 2) == 0) {
    data = strtol(argv[4], nullptr, 0);
  } else {
    std::cout << "adding " << argv[4] << " into string pool" << std::endl;
    auto ensure_result =
        ensure_string_in_xml_string_pool(path, std::string(argv[4]));
    if (ensure_result < 0) {
      return 1;
    }
    data = (uint32_t)ensure_result;
    std::cout << "finished appending string pool with new idx " << data
              << std::endl;
  }

  auto map = std::make_unique<boost::iostreams::mapped_file>();
  auto mode = (std::ios_base::openmode)(std::ios_base::in | std::ios_base::out);
  map->open(path, mode);
  if (!map->is_open()) {
    std::cerr << "Could not map " << path << std::endl;
    return 1;
  }

  XmlAttributeSetter setter(tag_name, is_using_attr_id, argv[3], attribute_id,
                            data);
  LOG_ALWAYS_FATAL_IF(!setter.visit(map->data(), map->size()),
                      "Failed to parse file %s", path.c_str());
  return 0;
}
