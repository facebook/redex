/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/iostreams/device/mapped_file.hpp>
#include <iostream>

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Visitor.h"

namespace {
class XmlAttributeSetter : public arsc::XmlFileVisitor {
 public:
  ~XmlAttributeSetter() override {}

  XmlAttributeSetter(const std::string& tag_name,
                     uint32_t attribute_id,
                     uint32_t data)
      : m_tag_name(tag_name), m_attribute_id(attribute_id), m_data(data) {}

  bool visit_global_strings(android::ResStringPool_header* pool) override {
    LOG_ALWAYS_FATAL_IF(m_global_strings.setTo(pool, dtohl(pool->header.size),
                                               true) != android::NO_ERROR,
                        "Invalid string pool");
    return true;
  }

  bool visit_attribute_ids(uint32_t* id, size_t count) override {
    m_ids = id;
    m_attribute_count = count;
    return true;
  }

  bool visit_start_tag(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension) override {
    auto idx = dtohl(extension->name.index);
    size_t len;
    auto chars = m_global_strings.stringAt(idx, &len);
    if (chars != nullptr) {
      android::String16 s16(chars, len);
      android::String8 s8(s16);
      auto current_name = std::string(s8.string());
      m_found_tag = m_tag_name == current_name;
    } else {
      m_found_tag = false;
    }
    return XmlFileVisitor::visit_start_tag(node, extension);
  }

  bool visit_attribute(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension,
                       android::ResXMLTree_attribute* attribute) override {
    if (m_found_tag) {
      auto attr_idx = dtohl(attribute->name.index);
      if (attr_idx < m_attribute_count && m_ids[attr_idx] == m_attribute_id) {
        std::cout << "Found target attribute 0x" << std::hex << m_attribute_id
                  << " at file offset 0x" << get_file_offset(attribute)
                  << std::endl;
        attribute->typedValue.data = m_data;
      }
    }
    return arsc::XmlFileVisitor::visit_attribute(node, extension, attribute);
  }

  // Item to find and data to set
  std::string m_tag_name;
  uint32_t m_attribute_id;
  uint32_t m_data;
  // Parsed structures in the file
  android::ResStringPool m_global_strings;
  uint32_t* m_ids;
  uint32_t m_attribute_count;
  // State
  bool m_found_tag = false;
};
} // namespace

// This tool accepts a tag name, attribute ID as defined in the Android SDK, see
// https://cs.android.com/android/platform/superproject/+/android-12.0.0_r1:prebuilts/sdk/3/public/api/android.txt;l=344
// and the raw bytes to set for the attribute's value.
int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "Usage: xml_editor AndroidManifest.xml application 0x101000c "
                 "0xffffffff\n";
    return 1;
  }
  auto path = std::string(argv[1]);
  auto tag_name = std::string(argv[2]);
  auto attribute_id = strtol(argv[3], nullptr, 0);
  auto data = strtol(argv[4], nullptr, 0);

  auto map = std::make_unique<boost::iostreams::mapped_file>();
  auto mode = (std::ios_base::openmode)(std::ios_base::in | std::ios_base::out);
  map->open(path, mode);
  if (!map->is_open()) {
    std::cerr << "Could not map " << path << std::endl;
    return 1;
  }

  XmlAttributeSetter setter(tag_name, attribute_id, data);
  LOG_ALWAYS_FATAL_IF(!setter.visit(map->data(), map->size()),
                      "Failed to parse file %s", path.c_str());
  return 0;
}
