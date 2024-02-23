/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/optional/optional_io.hpp>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <secure_lib/secure_string.h>
#include <utility>

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/Vector.h"
#include "utils/Visitor.h"

constexpr uint32_t ID_ATTRIBUTE = 0x010100d0;

namespace {

// give a stringPool ref of a name, return name in string format.
boost::optional<std::string> get_name_string(
    const struct android::ResStringPool_ref& name_ref,
    android::ResStringPool& pool) {
  auto idx = dtohl(name_ref.index);
  size_t len;
  auto chars = pool.stringAt(idx, &len);
  if (chars != nullptr) {
    android::String16 s16(chars, len);
    android::String8 s8(s16);
    return std::string(s8.string());
  } else {
    return boost::none;
  }
}

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
    arsc::write_bytes_to_file(new_bytes, path);
  }
  return idx;
}

class XmlValidator : public arsc::SimpleXmlParser {
 public:
  ~XmlValidator() override {}

  XmlValidator(uint32_t attribute_id,
               std::string attribute_name,
               std::string attribute_namespace,
               std::string tag_name,
               bool is_using_attr_id)
      : m_attribute_id(attribute_id),
        m_attribute_name(std::move(attribute_name)),
        m_attribute_namespace(std::move(attribute_namespace)),
        m_node_name(std::move(tag_name)),
        m_is_using_attr_id(is_using_attr_id) {}

  bool visit_attribute_ids(android::ResChunk_header* header,
                           uint32_t* id,
                           size_t count) override {
    if (m_is_using_attr_id) {
      for (size_t i = 0; i < count; i++) {
        if (id[i] == m_attribute_id &&
            arsc::get_string_from_pool(global_strings(), i) !=
                m_attribute_name) {
          m_id_name_match = false;
        }
      }
    }
    m_ids = id;
    return arsc::SimpleXmlParser::visit_attribute_ids(header, id, count);
  }

  bool visit_start_tag(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension) override {
    auto current_name = get_name_string(extension->name, global_strings());
    // you cannot give a empty string as m_tag_name
    m_found_node = m_node_name == current_name;
    return SimpleXmlParser::visit_start_tag(node, extension);
  }

  bool visit_attribute(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension,
                       android::ResXMLTree_attribute* attribute) override {
    if (m_found_node) {
      boost::optional<std::string> attr_name =
          get_name_string(attribute->name, global_strings());
      auto attr_idx = dtohl(attribute->name.index);
      if (m_is_using_attr_id && attr_idx < attribute_count() &&
          m_ids[attr_idx] == m_attribute_id) {
        m_found_attribute = true;
      } else if (attr_name == m_attribute_name) {
        m_found_attribute = true;
      }
    }
    return arsc::SimpleXmlParser::visit_attribute(node, extension, attribute);
  }

  bool visit_start_namespace(
      android::ResXMLTree_node* node,
      android::ResXMLTree_namespaceExt* extension) override {

    if (arsc::get_string_from_pool(this->global_strings(),
                                   extension->prefix.index) ==
        m_attribute_namespace) {
      m_namespace_found = true;
    }
    return arsc::SimpleXmlParser::visit_start_namespace(node, extension);
  }

  uint32_t m_attribute_id;

  std::string m_attribute_name;
  std::string m_attribute_namespace;
  std::string m_node_name;
  bool m_is_using_attr_id;
  uint32_t* m_ids;

  bool m_found_attribute = false;
  bool m_namespace_found = false;
  bool m_found_node = false;
  bool m_id_name_match = true;
};

class XmlBuilder {
 public:
  XmlBuilder(uint32_t attribute_id,
             std::string attribute_name,
             std::string attribute_value,
             uint8_t data_type,
             std::string attribute_namespace,
             std::string node_name,
             bool is_using_attr_id)
      : m_attribute_id(attribute_id),
        m_attribute_name(std::move(attribute_name)),
        m_attribute_value(std::move(attribute_value)),
        m_data_type(data_type),
        m_attribute_namespace(std::move(attribute_namespace)),
        m_node_name(std::move(node_name)),
        m_is_using_attr_id(is_using_attr_id) {}

  void serialize(android::ResChunk_header* data,
                 size_t len,
                 android::Vector<char>* out) {
    auto file_manipulator = arsc::ResFileManipulator((char*)data, len);

    if (!m_node_name.empty()) {
      auto new_data_size = 2 * sizeof(android::ResXMLTree_node) +
                           sizeof(android::ResXMLTree_attrExt) +
                           sizeof(android::ResXMLTree_attribute) +
                           sizeof(android::ResXMLTree_endElementExt);
      arsc::ResFileManipulator::Block block(new_data_size);

      android::ResChunk_header node_header;
      node_header.headerSize = sizeof(android::ResXMLTree_node);
      node_header.size = sizeof(android::ResXMLTree_node) +
                         sizeof(android::ResXMLTree_attrExt) +
                         sizeof(android::ResXMLTree_attribute);
      node_header.type = android::RES_XML_START_ELEMENT_TYPE;

      android::ResStringPool_ref node_ref;
      node_ref.index = 0xFFFFFFFF;

      android::ResXMLTree_node node;
      node.header = node_header;
      node.lineNumber = 0;
      node.comment = node_ref;
      block.write(node);

      android::ResXMLTree_attrExt attr_ext;
      attr_ext.attributeCount = 1;
      attr_ext.attributeSize = sizeof(android::ResXMLTree_attribute);
      attr_ext.attributeStart = sizeof(android::ResXMLTree_attrExt);
      attr_ext.classIndex = 0;
      attr_ext.idIndex = 0;
      attr_ext.styleIndex = 0;
      attr_ext.name = m_node_name_ref;
      attr_ext.ns = node_ref;
      block.write(attr_ext);

      block.write(m_attribute);

      android::ResChunk_header end_node_header;
      end_node_header.headerSize = sizeof(android::ResXMLTree_node);
      end_node_header.size = sizeof(android::ResXMLTree_node) +
                             sizeof(android::ResXMLTree_endElementExt);
      end_node_header.type = android::RES_XML_END_ELEMENT_TYPE;

      android::ResXMLTree_node end_node;
      end_node.header = end_node_header;
      end_node.lineNumber = 0;
      end_node.comment = node_ref;
      block.write(end_node);

      android::ResXMLTree_endElementExt end_node_ext;
      end_node_ext.name = m_node_name_ref;
      end_node_ext.ns = node_ref;
      block.write(end_node_ext);

      auto insert_pos = (char*)data + m_new_data_offset;
      file_manipulator.add_at(insert_pos, std::move(block));
    } else {
      android::ResXMLTree_node* node =
          (android::ResXMLTree_node*)((char*)data + m_node_offset);

      file_manipulator.replace_at(
          (char*)node + sizeof(uint32_t),
          node->header.size + sizeof(android::ResXMLTree_attribute));
      file_manipulator.add_at((char*)data + m_new_data_offset, m_attribute);
    }

    file_manipulator.serialize(out);
  }

  ssize_t add_attribute_and_node_properties(const std::string& path) {
    android::Vector<char> new_bytes;
    size_t idx{0};
    ssize_t result;
    auto map = std::make_unique<boost::iostreams::mapped_file>();
    auto mode = (std::ios_base::openmode)std::ios_base::in;
    map->open(path, mode);

    auto res = arsc::ensure_attribute_in_xml_doc(
        map->const_data(), map->size(), m_attribute_name, m_attribute_id,
        &new_bytes, &idx);
    if (res != android::OK) {
      return 1;
    }
    m_attribute.name.index = idx;

    if (!new_bytes.empty()) {
      arsc::write_bytes_to_file(new_bytes, path);
    }

    if (!m_attribute_namespace.empty()) {
      result = ensure_string_in_xml_string_pool(path, m_attribute_namespace);
      if (result < 0) {
        return 1;
      }
      m_attribute.ns.index = result;
    } else {
      m_attribute.ns.index = 0xFFFFFFFF;
    }

    android::Res_value typedValue;
    typedValue.size = sizeof(android::Res_value);
    typedValue.res0 = 0;
    typedValue.dataType = m_data_type;
    if (m_data_type == android::Res_value::TYPE_STRING) {
      result = ensure_string_in_xml_string_pool(path, m_attribute_value);
      if (result < 0) {
        return 1;
      }
      typedValue.data = result;
    } else {
      typedValue.data = std::stol(m_attribute_value, nullptr, 0);
    }
    m_attribute.typedValue = typedValue;

    if (m_attribute.typedValue.dataType == android::Res_value::TYPE_STRING) {
      m_attribute.rawValue.index = result;
    } else {
      m_attribute.rawValue.index = 0xFFFFFFFF;
    }

    if (!m_node_name.empty()) {
      result = ensure_string_in_xml_string_pool(path, m_node_name);
      if (result < 0) {
        return 1;
      }
      m_node_name_ref.index = result;
    }
    return android::OK;
  }

  bool inserted_data() { return m_new_data_offset != 0; }

  uint32_t m_attribute_id;
  std::string m_attribute_name;
  std::string m_attribute_value;
  uint8_t m_data_type;
  std::string m_attribute_namespace;

  android::ResXMLTree_attribute m_attribute;

  uint32_t m_new_data_offset = 0;
  uint32_t m_node_offset = 0;
  std::string m_node_name;
  android::ResStringPool_ref m_node_name_ref;
  bool m_is_using_attr_id;
};

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

  XmlAttributeSetter(const std::string& tag_name,
                     bool is_using_attr_id,
                     const char* attribute_input,
                     uint32_t attribute_id,
                     uint32_t data_input,
                     XmlBuilder* xml_builder)
      : m_tag_name(tag_name),
        m_is_using_attr_id(is_using_attr_id),
        m_attribute(attribute_input),
        m_attribute_id(attribute_id),
        m_data(data_input),
        m_xml_builder(xml_builder) {
    m_inserting_attribute = true;
    if (!m_xml_builder->m_node_name.empty()) {
      m_inserting_node = true;
    }
  }

  bool visit_start_tag(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension) override {
    auto current_name = get_name_string(extension->name, global_strings());
    // you cannot give a empty string as m_tag_name
    m_found_tag = m_tag_name == current_name;
    ssize_t ordinal;
    bool inserted_attribute = false;
    if (m_found_tag && m_inserting_attribute) {
      if (!m_inserting_node) {
        auto pool_lookup = [&](uint32_t idx) {
          auto& pool = global_strings();
          return arsc::get_string_from_pool(pool, idx);
        };
        ordinal = arsc::find_attribute_ordinal(node, extension,
                                               &m_xml_builder->m_attribute,
                                               attribute_count(), pool_lookup);
        if (!m_xml_builder->inserted_data()) {
          m_xml_builder->m_new_data_offset =
              get_file_offset(extension) + extension->attributeStart +
              sizeof(android::ResXMLTree_attribute) * ordinal;
          m_xml_builder->m_node_offset = get_file_offset(node);
          inserted_attribute = true;
        }
      } else if (!m_xml_builder->inserted_data()) {
        m_xml_builder->m_new_data_offset =
            get_file_offset(extension) + extension->attributeStart +
            sizeof(android::ResXMLTree_attribute) * extension->attributeCount;
        m_xml_builder->m_node_offset = get_file_offset(node);
      }
    }
    bool res = SimpleXmlParser::visit_start_tag(node, extension);
    if (inserted_attribute) {
      extension->attributeCount += 1;
      if (m_attribute_id == ID_ATTRIBUTE) {
        extension->idIndex = ordinal;
      } else if (m_xml_builder->m_attribute_name == "class") {
        extension->classIndex = ordinal;
      } else if (m_xml_builder->m_attribute_name == "style") {
        extension->styleIndex = ordinal;
      } else { // inserting a normal attribute
        if (extension->idIndex >= ordinal) {
          extension->idIndex += 1;
        }
        if (extension->classIndex >= ordinal) {
          extension->classIndex += 1;
        }
        if (extension->styleIndex >= ordinal) {
          extension->styleIndex += 1;
        }
      }
    }
    return res;
  }

  bool visit_attribute(android::ResXMLTree_node* node,
                       android::ResXMLTree_attrExt* extension,
                       android::ResXMLTree_attribute* attribute) override {
    if (m_found_tag && !m_edited_attribute) {
      boost::optional<std::string> attr_name =
          get_name_string(attribute->name, global_strings());
      auto attr_idx = dtohl(attribute->name.index);
      bool found_attribute = false;
      if (m_is_using_attr_id && attr_idx < attribute_count() &&
          get_attribute_id(attr_idx) == m_attribute_id) {
        std::cout << "Found target attribute 0x" << std::hex
                  << get_attribute_id(attr_idx) << " at file offset 0x"
                  << get_file_offset(attribute) << std::endl;
        found_attribute = true;
        m_edited_attribute = true;
      } else if (attr_name && *attr_name == m_attribute) {
        std::cout << "Found target attribute " << m_attribute
                  << " at file offset 0x" << get_file_offset(attribute)
                  << std::endl;
        found_attribute = true;
        m_edited_attribute = true;
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
  // State
  bool m_found_tag = false;
  bool m_edited_attribute = false;

  bool m_inserting_attribute = false;
  bool m_inserting_node = false;
  XmlBuilder* m_xml_builder;
};
} // namespace

size_t edit_attribute(char* attribute_name,
                      char* attribute_value,
                      const std::basic_string<char>& path,
                      const std::basic_string<char>& tag_name,
                      bool is_using_attr_id,
                      uint32_t attribute_id) {
  uint32_t data;
  if (checked_strncmp(attribute_value, strlen(attribute_value), "0x", 2, 2) ==
      0) {
    data = strtol(attribute_value, nullptr, 0);
  } else {
    std::cout << "adding " << attribute_value << " into string pool"
              << std::endl;
    auto ensure_result =
        ensure_string_in_xml_string_pool(path, std::string(attribute_value));
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

  XmlAttributeSetter setter(tag_name, is_using_attr_id, attribute_name,
                            attribute_id, data);
  LOG_ALWAYS_FATAL_IF(!setter.visit(map->data(), map->size()),
                      "Failed to parse file %s", path.c_str());
  return 0;
}

// This tool accepts a tag name, attribute ID as defined in the Android SDK, see
// https://cs.android.com/android/platform/superproject/+/android-12.0.0_r1:prebuilts/sdk/3/public/api/android.txt;l=344
// and the raw bytes to set for the attribute's value.
int main(int argc, char** argv) {
  if (argc < 5 || argc > 9) {
    std::cerr << "Usage: xml_editor AndroidManifest.xml <tag_name> <attribute "
                 "id/name> "
                 "<new attribute value in string/hex num> or \n"
                 "xml_editor AndroidManifest.xml <tag_name> <attribute "
                 "name> "
                 "<new attribute value in string/hex num> "
                 "<attribute id> "
                 "<attribute type> "
                 "<attribute namespace> "
                 "<node name>\n";
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
  if (argc == 5) {
    return edit_attribute(argv[3], argv[4], path, tag_name, is_using_attr_id,
                          attribute_id);
  } else if (argc <= 9) {
    if (checked_strncmp(argv[5], strlen(argv[3]), "0x", 2, 2) == 0) {
      is_using_attr_id = true;
      attribute_id = (uint32_t)(strtol(argv[5], nullptr, 0));
    }
    auto type_arg = strtoul(argv[6], nullptr, 0);
    if (type_arg > android::Res_value::TYPE_LAST_INT) {
      std::cerr << "The attribute type value must be at most "
                << android::Res_value::TYPE_LAST_INT << std::endl;
      return 1;
    }
    uint8_t value_type = static_cast<uint8_t>(type_arg);
    std::basic_string<char> attribute_namespace;
    std::basic_string<char> node_name;
    if (argc >= 8) {
      attribute_namespace = std::string(argv[7]);
    }
    if (argc == 9) {
      node_name = std::string(argv[8]);
    }

    auto map = std::make_unique<boost::iostreams::mapped_file>();
    auto mode =
        (std::ios_base::openmode)(std::ios_base::in | std::ios_base::out);
    map->open(path, mode);
    if (!map->is_open()) {
      std::cerr << "Could not map " << path << std::endl;
      return 1;
    }

    XmlValidator validator(attribute_id, argv[3], attribute_namespace, tag_name,
                           is_using_attr_id);
    LOG_ALWAYS_FATAL_IF(!validator.visit(map->data(), map->size()),
                        "Failed to parse file %s", path.c_str());
    map->close();

    if (!attribute_namespace.empty() && !validator.m_namespace_found) {
      std::cerr << "Namespace does not exist in file" << std::endl;
      return 1;
    }
    if (!validator.m_id_name_match) {
      std::cerr << "An existing attribute with the same id has a different name"
                << std::endl;
      return 1;
    }

    // if the attribute was found, then use edit it instead of inserting it
    if (validator.m_found_attribute) {
      return edit_attribute(argv[3], argv[4], path, tag_name, is_using_attr_id,
                            attribute_id);
    }

    XmlBuilder xml_builder(attribute_id, argv[3], argv[4], value_type,
                           attribute_namespace, node_name, is_using_attr_id);

    auto builder_ok = xml_builder.add_attribute_and_node_properties(path);
    if (builder_ok < 0) {
      return 1;
    }
    map->open(path, mode);
    if (!map->is_open()) {
      std::cerr << "Could not map " << path << std::endl;
      return 1;
    }

    XmlAttributeSetter setter(tag_name, is_using_attr_id, argv[3], attribute_id,
                              xml_builder.m_attribute.typedValue.data,
                              &xml_builder);
    LOG_ALWAYS_FATAL_IF(!setter.visit(map->data(), map->size()),
                        "Failed to parse file %s", path.c_str());

    android::Vector<char> output;
    xml_builder.serialize((android::ResChunk_header*)((char*)map->data()),
                          map->size(), &output);
    arsc::write_bytes_to_file(output, path);
  }
  return 0;
}
