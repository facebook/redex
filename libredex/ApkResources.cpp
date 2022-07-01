/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApkResources.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>
#include <fcntl.h>
#include <map>

#include "Macros.h"
#if IS_WINDOWS
#include "CompatWindows.h"
#include <io.h>
#include <share.h>
#endif

#include "androidfw/ResourceTypes.h"
#include "androidfw/TypeWrappers.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/TypeHelpers.h"
#include "utils/Visitor.h"

#include "Debug.h"
#include "DexUtil.h"
#include "IOUtil.h"
#include "ReadMaybeMapped.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "Trace.h"
#include "WorkQueue.h"

// Workaround for inclusion order, when compiling on Windows (#defines NO_ERROR
// as 0).
#ifdef NO_ERROR
#undef NO_ERROR
#endif

namespace apk {
std::string get_string_from_pool(const android::ResStringPool& pool,
                                 size_t idx) {
  size_t u16_len;
  auto wide_chars = pool.stringAt(idx, &u16_len);
  android::String16 s16(wide_chars, u16_len);
  android::String8 string8(s16);
  return std::string(string8.string());
}

bool TableParser::visit_global_strings(android::ResStringPool_header* pool) {
  m_global_pool_header = pool;
  arsc::StringPoolRefVisitor::visit_global_strings(pool);
  return true;
}

bool TableParser::visit_package(android::ResTable_package* package) {
  m_packages.emplace(package);
  std::vector<android::ResChunk_header*> headers;
  m_package_unknown_chunks.emplace(package, std::move(headers));
  arsc::StringPoolRefVisitor::visit_package(package);
  return true;
}

bool TableParser::visit_key_strings(android::ResTable_package* package,
                                    android::ResStringPool_header* pool) {
  m_package_key_string_headers.emplace(package, pool);
  arsc::StringPoolRefVisitor::visit_key_strings(package, pool);
  return true;
}

bool TableParser::visit_type_strings(android::ResTable_package* package,
                                     android::ResStringPool_header* pool) {
  m_package_type_string_headers.emplace(package, pool);
  arsc::StringPoolRefVisitor::visit_type_strings(package, pool);
  return true;
}

bool TableParser::visit_type_spec(android::ResTable_package* package,
                                  android::ResTable_typeSpec* type_spec) {
  arsc::TypeInfo info{type_spec, {}};
  auto search = m_package_types.find(package);
  if (search == m_package_types.end()) {
    std::vector<arsc::TypeInfo> infos;
    infos.emplace_back(info);
    m_package_types.emplace(package, infos);
  } else {
    search->second.emplace_back(info);
  }
  arsc::StringPoolRefVisitor::visit_type_spec(package, type_spec);
  return true;
}

bool TableParser::visit_type(android::ResTable_package* package,
                             android::ResTable_typeSpec* type_spec,
                             android::ResTable_type* type) {
  auto& infos = m_package_types.at(package);
  for (auto& info : infos) {
    if (info.spec->id == type_spec->id) {
      info.configs.emplace_back(type);
    }
  }
  arsc::StringPoolRefVisitor::visit_type(package, type_spec, type);
  return true;
}

bool TableParser::visit_unknown_chunk(android::ResTable_package* package,
                                      android::ResChunk_header* header) {
  auto& chunks = m_package_unknown_chunks.at(package);
  chunks.emplace_back(header);
  return true;
}

bool TableEntryParser::visit_type_spec(android::ResTable_package* package,
                                       android::ResTable_typeSpec* type_spec) {
  TableParser::visit_type_spec(package, type_spec);
  auto package_type_id = make_package_type_id(package, type_spec->id);
  m_types.emplace(package_type_id, type_spec);
  TypeToEntries map;
  m_types_to_entries.emplace(package, std::move(map));
  std::vector<android::ResTable_type*> vec;
  m_types_to_configs.emplace(package_type_id, std::move(vec));
  return true;
}

void TableEntryParser::put_entry_data(uint32_t res_id,
                                      android::ResTable_package* package,
                                      android::ResTable_type* type,
                                      arsc::EntryValueData& data) {
  {
    auto search = m_res_id_to_entries.find(res_id);
    if (search == m_res_id_to_entries.end()) {
      ConfigToEntry c;
      m_res_id_to_entries.emplace(res_id, std::move(c));
    }
    auto& c = m_res_id_to_entries.at(res_id);
    c.emplace(&type->config, data);
  }
  {
    auto& map = m_types_to_entries.at(package);
    auto search = map.find(type);
    if (search == map.end()) {
      std::vector<arsc::EntryValueData> vec;
      map.emplace(type, std::move(vec));
    }
    auto& vec = map.at(type);
    vec.emplace_back(data);
  }
}

bool TableEntryParser::visit_type(android::ResTable_package* package,
                                  android::ResTable_typeSpec* type_spec,
                                  android::ResTable_type* type) {
  TableParser::visit_type(package, type_spec, type);
  auto package_type_id = make_package_type_id(package, type_spec->id);
  auto& types = m_types_to_configs.at(package_type_id);
  types.emplace_back(type);
  android::TypeVariant tv(type);
  uint16_t entry_id = 0;
  for (auto it = tv.beginEntries(); it != tv.endEntries(); ++it, ++entry_id) {
    android::ResTable_entry* entry = const_cast<android::ResTable_entry*>(*it);
    uint32_t res_id = package_type_id << 16 | entry_id;
    if (entry == nullptr) {
      arsc::EntryValueData data(nullptr, 0);
      put_entry_data(res_id, package, type, data);
    } else {
      auto size = arsc::compute_entry_value_length(entry);
      arsc::EntryValueData data((uint8_t*)entry, size);
      put_entry_data(res_id, package, type, data);
    }
    m_res_id_to_flags.emplace(res_id,
                              arsc::get_spec_flags(type_spec, entry_id));
  }
  return true;
}

} // namespace apk

namespace {

#define MAKE_RES_ID(package, type, entry)                        \
  ((PACKAGE_MASK_BIT & ((package) << PACKAGE_INDEX_BIT_SHIFT)) | \
   (TYPE_MASK_BIT & ((type) << TYPE_INDEX_BIT_SHIFT)) |          \
   (ENTRY_MASK_BIT & (entry)))

void ensure_file_contents(const std::string& file_contents,
                          const std::string& filename) {
  if (file_contents.empty()) {
    fprintf(stderr, "Unable to read file: %s\n", filename.data());
    throw std::runtime_error("Unable to read file: " + filename);
  }
}

/*
 * Reads an entire file into a std::string. Returns an empty string if
 * anything went wrong (e.g. file not found).
 */
std::string read_entire_file(const std::string& filename) {
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  std::ostringstream sstr;
  sstr << in.rdbuf();
  redex_assert(!in.bad());
  return sstr.str();
}

size_t write_serialized_data(const android::Vector<char>& cVec,
                             RedexMappedFile f) {
  size_t vec_size = cVec.size();
  size_t f_size = f.size();
  if (vec_size > 0) {
    memcpy(f.data(), &(cVec[0]), vec_size);
  }
  f.file.reset(); // Close the map.
#if IS_WINDOWS
  int fd;
  auto open_res =
      _sopen_s(&fd, f.filename.c_str(), _O_BINARY | _O_RDWR, _SH_DENYRW, 0);
  redex_assert(open_res == 0);
  auto trunc_res = _chsize_s(fd, vec_size);
  _close(fd);
#else
  auto trunc_res = truncate(f.filename.c_str(), vec_size);
#endif
  redex_assert(trunc_res == 0);
  return vec_size > 0 ? vec_size : f_size;
}

/*
 * Look for <search_tag> within the descendants of the current node in the XML
 * tree.
 */
bool find_nested_tag(const android::String16& search_tag,
                     android::ResXMLTree* parser) {
  size_t depth{1};
  while (depth) {
    auto type = parser->next();
    switch (type) {
    case android::ResXMLParser::START_TAG: {
      ++depth;
      size_t len;
      android::String16 tag(parser->getElementName(&len));
      if (tag == search_tag) {
        return true;
      }
      break;
    }
    case android::ResXMLParser::END_TAG: {
      --depth;
      break;
    }
    case android::ResXMLParser::BAD_DOCUMENT: {
      not_reached();
    }
    default: {
      break;
    }
    }
  }
  return false;
}

/*
 * Parse AndroidManifest from buffer, return a list of class names that are
 * referenced
 */
ManifestClassInfo extract_classes_from_manifest(const char* data, size_t size) {
  // Tags
  android::String16 activity("activity");
  android::String16 activity_alias("activity-alias");
  android::String16 application("application");
  android::String16 provider("provider");
  android::String16 receiver("receiver");
  android::String16 service("service");
  android::String16 instrumentation("instrumentation");
  android::String16 intent_filter("intent-filter");

  // This is not an unordered_map because String16 doesn't define a hash
  std::map<android::String16, ComponentTag> string_to_tag{
      {activity, ComponentTag::Activity},
      {activity_alias, ComponentTag::ActivityAlias},
      {provider, ComponentTag::Provider},
      {receiver, ComponentTag::Receiver},
      {service, ComponentTag::Service},
  };

  // Attributes
  android::String16 authorities("authorities");
  android::String16 exported("exported");
  android::String16 protection_level("protectionLevel");
  android::String16 permission("permission");
  android::String16 name("name");
  android::String16 target_activity("targetActivity");
  android::String16 app_component_factory("appComponentFactory");

  android::ResXMLTree parser;
  parser.setTo(data, size);

  ManifestClassInfo manifest_classes;

  if (parser.getError() != android::NO_ERROR) {
    return manifest_classes;
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      if (tag == application) {
        std::string classname = get_string_attribute_value(parser, name);
        // android:name is an optional attribute for <application>
        if (!classname.empty()) {
          manifest_classes.application_classes.emplace(
              java_names::external_to_internal(classname));
        }
        std::string app_factory_cls =
            get_string_attribute_value(parser, app_component_factory);
        if (!app_factory_cls.empty()) {
          manifest_classes.application_classes.emplace(
              java_names::external_to_internal(app_factory_cls));
        }
      } else if (tag == instrumentation) {
        std::string classname = get_string_attribute_value(parser, name);
        always_assert(classname.size());
        manifest_classes.instrumentation_classes.emplace(
            java_names::external_to_internal(classname));
      } else if (string_to_tag.count(tag)) {
        std::string classname = get_string_attribute_value(
            parser, tag != activity_alias ? name : target_activity);
        always_assert(classname.size());

        bool has_exported_attribute = has_bool_attribute(parser, exported);
        android::Res_value ignore_output;
        bool has_permission_attribute =
            has_raw_attribute_value(parser, permission, ignore_output);
        bool has_protection_level_attribute =
            has_raw_attribute_value(parser, protection_level, ignore_output);
        bool is_exported = get_bool_attribute_value(parser, exported,
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
        std::string permission_attribute;
        std::string protection_level_attribute;
        if (has_permission_attribute) {
          permission_attribute = get_string_attribute_value(parser, permission);
        }
        if (has_protection_level_attribute) {
          protection_level_attribute =
              get_string_attribute_value(parser, protection_level);
        }

        ComponentTagInfo tag_info(string_to_tag.at(tag),
                                  java_names::external_to_internal(classname),
                                  export_attribute,
                                  permission_attribute,
                                  protection_level_attribute);

        if (tag == provider) {
          std::string text = get_string_attribute_value(parser, authorities);
          parse_authorities(text, &tag_info.authority_classes);
        } else {
          tag_info.has_intent_filters = find_nested_tag(intent_filter, &parser);
        }

        manifest_classes.component_tags.emplace_back(tag_info);
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  return manifest_classes;
}

std::string convert_from_string16(const android::String16& string16) {
  android::String8 string8(string16);
  std::string converted(string8.string());
  return converted;
}
} // namespace

// Returns the attribute with the given name for the current XML element
std::string get_string_attribute_value(
    const android::ResXMLTree& parser,
    const android::String16& attribute_name) {

  const size_t attr_count = parser.getAttributeCount();

  for (size_t i = 0; i < attr_count; ++i) {
    size_t len;
    android::String16 key(parser.getAttributeName(i, &len));
    if (key == attribute_name) {
      const char16_t* p = parser.getAttributeStringValue(i, &len);
      if (p != nullptr) {
        android::String16 target(p, len);
        std::string converted = convert_from_string16(target);
        return converted;
      }
    }
  }
  return std::string("");
}

bool has_raw_attribute_value(const android::ResXMLTree& parser,
                             const android::String16& attribute_name,
                             android::Res_value& out_value) {
  const size_t attr_count = parser.getAttributeCount();

  for (size_t i = 0; i < attr_count; ++i) {
    size_t len;
    android::String16 key(parser.getAttributeName(i, &len));
    if (key == attribute_name) {
      parser.getAttributeValue(i, &out_value);
      return true;
    }
  }

  return false;
}

bool has_bool_attribute(const android::ResXMLTree& parser,
                        const android::String16& attribute_name) {
  android::Res_value raw_value;
  if (has_raw_attribute_value(parser, attribute_name, raw_value)) {
    return raw_value.dataType == android::Res_value::TYPE_INT_BOOLEAN;
  }
  return false;
}

bool get_bool_attribute_value(const android::ResXMLTree& parser,
                              const android::String16& attribute_name,
                              bool default_value) {
  android::Res_value raw_value;
  if (has_raw_attribute_value(parser, attribute_name, raw_value)) {
    if (raw_value.dataType == android::Res_value::TYPE_INT_BOOLEAN) {
      return static_cast<bool>(raw_value.data);
    }
  }
  return default_value;
}

int get_int_attribute_or_default_value(const android::ResXMLTree& parser,
                                       const android::String16& attribute_name,
                                       int32_t default_value) {
  android::Res_value raw_value;
  if (has_raw_attribute_value(parser, attribute_name, raw_value)) {
    if (raw_value.dataType == android::Res_value::TYPE_INT_DEC) {
      return static_cast<int>(raw_value.data);
    }
  }
  return default_value;
}

std::vector<std::string> ApkResources::find_res_directories() {
  return {m_directory + "/res"};
}

std::vector<std::string> ApkResources::find_lib_directories() {
  return {m_directory + "/lib"};
}

std::string ApkResources::get_base_assets_dir() {
  return m_directory + "/assets";
}

namespace {
bool is_binary_xml(const void* data, size_t size) {
  if (size < sizeof(android::ResChunk_header)) {
    return false;
  }
  return dtohs(((android::ResChunk_header*)data)->type) ==
         android::RES_XML_TYPE;
}

std::string read_attribute_name_at_idx(const android::ResXMLTree& parser,
                                       size_t idx) {
  size_t len;
  auto name_chars = parser.getAttributeName8(idx, &len);
  if (name_chars != nullptr) {
    return std::string(name_chars);
  } else {
    auto wide_chars = parser.getAttributeName(idx, &len);
    android::String16 s16(wide_chars, len);
    auto converted = convert_from_string16(s16);
    return converted;
  }
}

void extract_classes_from_layout(
    const char* data,
    size_t size,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>* out_classes,
    std::unordered_multimap<std::string, std::string>* out_attributes) {
  if (!is_binary_xml(data, size)) {
    return;
  }

  android::ResXMLTree parser;
  parser.setTo(data, size);

  android::String16 name("name");
  android::String16 klazz("class");
  android::String16 target_class("targetClass");

  if (parser.getError() != android::NO_ERROR) {
    return;
  }

  std::unordered_map<int, std::string> namespace_prefix_map;
  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      std::string classname = convert_from_string16(tag);
      if (!strcmp(classname.c_str(), "fragment") ||
          !strcmp(classname.c_str(), "view") ||
          !strcmp(classname.c_str(), "dialog") ||
          !strcmp(classname.c_str(), "activity") ||
          !strcmp(classname.c_str(), "intent")) {
        classname = get_string_attribute_value(parser, klazz);
        if (classname.empty()) {
          classname = get_string_attribute_value(parser, name);
        }
        if (classname.empty()) {
          classname = get_string_attribute_value(parser, target_class);
        }
      }
      std::string converted = std::string("L") + classname + std::string(";");

      bool is_classname = converted.find('.') != std::string::npos;
      if (is_classname) {
        std::replace(converted.begin(), converted.end(), '.', '/');
        out_classes->insert(converted);
      }
      if (!attributes_to_read.empty()) {
        for (size_t i = 0; i < parser.getAttributeCount(); i++) {
          auto ns_id = parser.getAttributeNamespaceID(i);
          std::string attr_name = read_attribute_name_at_idx(parser, i);
          std::string fully_qualified;
          if (ns_id >= 0) {
            fully_qualified = namespace_prefix_map[ns_id] + ":" + attr_name;
          } else {
            fully_qualified = attr_name;
          }
          if (attributes_to_read.count(fully_qualified) != 0) {
            auto val = parser.getAttributeStringValue(i, &len);
            if (val != nullptr) {
              android::String16 s16(val, len);
              out_attributes->emplace(fully_qualified,
                                      convert_from_string16(s16));
            }
          }
        }
      }
    } else if (type == android::ResXMLParser::START_NAMESPACE) {
      auto id = parser.getNamespaceUriID();
      size_t len;
      auto prefix = parser.getNamespacePrefix(&len);
      namespace_prefix_map.emplace(
          id, convert_from_string16(android::String16(prefix, len)));
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
}
} // namespace

void ApkResources::collect_layout_classes_and_attributes_for_file(
    const std::string& file_path,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>* out_classes,
    std::unordered_multimap<std::string, std::string>* out_attributes) {
  redex::read_file_with_contents(file_path, [&](const char* data, size_t size) {
    extract_classes_from_layout(data, size, attributes_to_read, out_classes,
                                out_attributes);
  });
}

boost::optional<int32_t> ApkResources::get_min_sdk() {
  if (!boost::filesystem::exists(m_manifest)) {
    return boost::none;
  }

  auto file = RedexMappedFile::open(m_manifest);

  if (file.size() == 0) {
    fprintf(stderr, "WARNING: Cannot find/read the manifest file %s\n",
            m_manifest.c_str());
    return boost::none;
  }

  android::ResXMLTree parser;
  parser.setTo(file.const_data(), file.size());

  if (parser.getError() != android::NO_ERROR) {
    fprintf(stderr, "WARNING: Failed to parse the manifest file %s\n",
            m_manifest.c_str());
    return boost::none;
  }

  const android::String16 uses_sdk("uses-sdk");
  const android::String16 min_sdk("minSdkVersion");
  android::ResXMLParser::event_code_t event_code;
  do {
    event_code = parser.next();
    if (event_code == android::ResXMLParser::START_TAG) {
      size_t outLen;
      auto el_name = android::String16(parser.getElementName(&outLen));
      if (el_name == uses_sdk) {
        android::Res_value raw_value;
        if (has_raw_attribute_value(parser, min_sdk, raw_value) &&
            (raw_value.dataType & android::Res_value::TYPE_INT_DEC)) {
          return boost::optional<int32_t>(static_cast<int32_t>(raw_value.data));
        } else {
          return boost::none;
        }
      }
    }
  } while ((event_code != android::ResXMLParser::END_DOCUMENT) &&
           (event_code != android::ResXMLParser::BAD_DOCUMENT));
  return boost::none;
}

ManifestClassInfo ApkResources::get_manifest_class_info() {
  std::string manifest =
      (boost::filesystem::path(m_directory) / "AndroidManifest.xml").string();
  ManifestClassInfo classes;
  if (boost::filesystem::exists(manifest)) {
    redex::read_file_with_contents(manifest, [&](const char* data,
                                                 size_t size) {
      if (size == 0) {
        fprintf(stderr, "Unable to read manifest file: %s\n", manifest.c_str());
        return;
      }
      classes = extract_classes_from_manifest(data, size);
    });
  }
  return classes;
}

std::unordered_set<uint32_t> ApkResources::get_xml_reference_attributes(
    const std::string& filename) {
  std::unordered_set<uint32_t> result;
  if (is_raw_resource(filename)) {
    return result;
  }
  auto file = RedexMappedFile::open(filename);
  android::ResXMLTree parser;
  parser.setTo(file.const_data(), file.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + filename);
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      const size_t attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; ++i) {
        if (parser.getAttributeDataType(i) ==
                android::Res_value::TYPE_REFERENCE ||
            parser.getAttributeDataType(i) ==
                android::Res_value::TYPE_ATTRIBUTE) {
          android::Res_value outValue;
          parser.getAttributeValue(i, &outValue);
          if (outValue.data > PACKAGE_RESID_START) {
            result.emplace(outValue.data);
          }
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  return result;
}

int ApkResources::replace_in_xml_string_pool(
    const void* data,
    const size_t len,
    const std::map<std::string, std::string>& rename_map,
    android::Vector<char>* out_data,
    size_t* out_num_renamed) {
  const auto chunk_size = sizeof(android::ResChunk_header);
  const auto pool_header_size = (uint16_t)sizeof(android::ResStringPool_header);

  // Validate the given bytes.
  if (len < chunk_size + pool_header_size) {
    return android::NOT_ENOUGH_DATA;
  }

  // Layout XMLs will have a ResChunk_header, followed by ResStringPool
  // representing each XML tag and attribute string.
  auto chunk = (android::ResChunk_header*)data;
  LOG_FATAL_IF(dtohl(chunk->size) != len, "Can't read header size");

  auto pool_ptr = (android::ResStringPool_header*)((char*)data + chunk_size);
  if (dtohs(pool_ptr->header.type) != android::RES_STRING_POOL_TYPE) {
    return android::BAD_TYPE;
  }

  size_t num_replaced = 0;
  android::ResStringPool pool(pool_ptr, dtohl(pool_ptr->header.size));

  // Straight copy of everything after the string pool.
  android::Vector<char> serialized_nodes;
  auto start = chunk_size + pool_ptr->header.size;
  auto remaining = len - start;
  serialized_nodes.resize(remaining);
  void* start_ptr = ((char*)data) + start;
  memcpy((void*)&serialized_nodes[0], start_ptr, remaining);

  // Rewrite the strings
  android::Vector<char> serialized_pool;
  auto num_strings = pool_ptr->stringCount;

  // Make an empty pool.
  auto new_pool_header = android::ResStringPool_header{
      {// Chunk type
       htods(android::RES_STRING_POOL_TYPE),
       // Header size
       htods(pool_header_size),
       // Total size (no items yet, equal to the header size)
       htodl(pool_header_size)},
      // String count
      0,
      // Style count
      0,
      // Flags (valid combinations of UTF8_FLAG, SORTED_FLAG)
      pool.isUTF8() ? htodl(android::ResStringPool_header::UTF8_FLAG)
                    : (uint32_t)0,
      // Offset from header to string data
      0,
      // Offset from header to style data
      0};
  android::ResStringPool new_pool(&new_pool_header, pool_header_size);

  for (size_t i = 0; i < num_strings; i++) {
    // Public accessors for strings are a bit of a foot gun. string8ObjectAt
    // does not reliably return lengths with chars outside the BMP. Work around
    // to get a proper String8.
    size_t u16_len;
    auto wide_chars = pool.stringAt(i, &u16_len);
    android::String16 s16(wide_chars, u16_len);
    android::String8 string8(s16);
    std::string existing_str(string8.string());

    auto replacement = rename_map.find(existing_str);
    if (replacement == rename_map.end()) {
      new_pool.appendString(string8);
    } else {
      android::String8 replacement8(replacement->second.c_str());
      new_pool.appendString(replacement8);
      num_replaced++;
    }
  }

  new_pool.serialize(serialized_pool);

  // Assemble
  arsc::push_short(android::RES_XML_TYPE, out_data);
  arsc::push_short(chunk_size, out_data);
  auto total_size =
      chunk_size + serialized_nodes.size() + serialized_pool.size();
  arsc::push_long(total_size, out_data);

  out_data->appendVector(serialized_pool);
  out_data->appendVector(serialized_nodes);

  *out_num_renamed = num_replaced;
  return android::OK;
}

bool ApkResources::rename_classes_in_layout(
    const std::string& file_path,
    const std::map<std::string, std::string>& rename_map,
    size_t* out_num_renamed) {
  RedexMappedFile f = RedexMappedFile::open(file_path, /* read_only= */ false);
  size_t len = f.size();

  android::Vector<char> serialized;
  auto status = replace_in_xml_string_pool(f.data(), f.size(), rename_map,
                                           &serialized, out_num_renamed);

  if (*out_num_renamed == 0) {
    return true;
  }
  if (status != android::OK) {
    return false;
  }
  write_serialized_data(serialized, std::move(f));
  return true;
}

std::unordered_set<std::string> ApkResources::find_all_xml_files() {
  std::string manifest_path = m_directory + "/AndroidManifest.xml";
  std::unordered_set<std::string> all_xml_files;
  all_xml_files.emplace(manifest_path);
  for (const std::string& path : get_xml_files(m_directory + "/res")) {
    all_xml_files.emplace(path);
  }
  return all_xml_files;
}

namespace {
bool is_drawable_attribute(android::ResXMLTree& parser, size_t attr_index) {
  size_t name_size;
  const char* attr_name_8 = parser.getAttributeName8(attr_index, &name_size);
  if (attr_name_8 != nullptr) {
    std::string name_str = std::string(attr_name_8, name_size);
    if (name_str.compare("drawable") == 0) {
      return true;
    }
  }

  const char16_t* attr_name_16 =
      parser.getAttributeName(attr_index, &name_size);
  if (attr_name_16 != nullptr) {
    android::String8 name_str_8 = android::String8(attr_name_16, name_size);
    std::string name_str = std::string(name_str_8.string(), name_size);
    if (name_str.compare("drawable") == 0) {
      return true;
    }
  }

  return false;
}

size_t getHashFromValues(const android::Vector<android::Res_value>& values) {
  size_t hash = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    boost::hash_combine(hash, values[i].data);
  }
  return hash;
}

} // namespace

int ApkResources::inline_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, android::Res_value>& id_to_inline_value) {
  int num_values_inlined = 0;
  std::string file_contents = read_entire_file(filename);
  ensure_file_contents(file_contents, filename);
  bool made_change = false;

  android::ResXMLTree parser;
  parser.setTo(file_contents.data(), file_contents.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + filename);
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      const size_t attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; ++i) {
        // Older versions of Android (below V5) do not allow inlining into
        // android:drawable attributes.
        if (is_drawable_attribute(parser, i)) {
          continue;
        }

        if (parser.getAttributeDataType(i) ==
            android::Res_value::TYPE_REFERENCE) {
          android::Res_value outValue;
          parser.getAttributeValue(i, &outValue);
          if (outValue.data <= PACKAGE_RESID_START) {
            continue;
          }

          auto p = id_to_inline_value.find(outValue.data);
          if (p != id_to_inline_value.end()) {
            android::Res_value new_value = p->second;
            parser.setAttribute(i, new_value);
            ++num_values_inlined;
            made_change = true;
          }
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  if (made_change) {
    write_string_to_file(filename, file_contents);
  }

  return num_values_inlined;
}

size_t ApkResources::remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) {
  if (is_raw_resource(filename)) {
    return 0;
  }
  std::string file_contents = read_entire_file(filename);
  ensure_file_contents(file_contents, filename);
  bool made_change = false;

  android::ResXMLTree parser;
  parser.setTo(file_contents.data(), file_contents.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + filename);
  }

  // Update embedded resource ID array
  size_t resIdCount = 0;
  uint32_t* resourceIds = parser.getResourceIds(&resIdCount);
  for (size_t i = 0; i < resIdCount; ++i) {
    auto id_search = kept_to_remapped_ids.find(resourceIds[i]);
    if (id_search != kept_to_remapped_ids.end()) {
      resourceIds[i] = id_search->second;
      made_change = true;
    }
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      const size_t attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; ++i) {
        if (parser.getAttributeDataType(i) ==
                android::Res_value::TYPE_REFERENCE ||
            parser.getAttributeDataType(i) ==
                android::Res_value::TYPE_ATTRIBUTE) {
          android::Res_value outValue;
          parser.getAttributeValue(i, &outValue);
          if (outValue.data > PACKAGE_RESID_START &&
              kept_to_remapped_ids.count(outValue.data)) {
            uint32_t new_value = kept_to_remapped_ids.at(outValue.data);
            if (new_value != outValue.data) {
              parser.setAttributeData(i, new_value);
              made_change = true;
            }
          }
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  if (made_change) {
    write_string_to_file(filename, file_contents);
  }
  return made_change;
}

std::unique_ptr<ResourceTableFile> ApkResources::load_res_table() {
  std::string arsc_path = m_directory + std::string("/resources.arsc");
  return std::make_unique<ResourcesArscFile>(arsc_path);
}

std::vector<std::string> ApkResources::find_resources_files() {
  return {m_directory + std::string("/resources.arsc")};
}

ApkResources::~ApkResources() {}

void ResourcesArscFile::remap_res_ids_and_serialize(
    const std::vector<std::string>& /* resource_files */,
    const std::map<uint32_t, uint32_t>& old_to_new) {
  remap_ids(old_to_new);
  serialize();
}

size_t ResourcesArscFile::obfuscate_resource_and_serialize(
    const std::vector<std::string>& /* unused */,
    const std::map<std::string, std::string>& filepath_old_to_new,
    const std::unordered_set<uint32_t>& allowed_types,
    const std::unordered_set<std::string>& keep_resource_prefixes) {
  always_assert_log(false, "ApkResource anonymizing not yet supported");
  return 0;
}

namespace {
const std::array<std::string, 2> KNOWN_RES_DIRS = {
    std::string(RES_DIRECTORY) + "/",
    std::string(OBFUSCATED_RES_DIRECTORY) + "/"};

// Checks relative paths that start with known resource file directories
// (supporting obfuscation).
bool is_resource_file(const std::string& str) {
  for (const auto& dir : KNOWN_RES_DIRS) {
    if (boost::algorithm::starts_with(str, dir)) {
      return true;
    }
  }
  return false;
}

// For the given package and type id, check if the type has any needed changes
// based on the old to new map. Output vector will contain the exhaustive
// mapping from new entry id (the index in the vec) to old entry id.
bool create_type_reordering(uint32_t package_id,
                            uint8_t type_id,
                            size_t entry_count,
                            const std::map<uint32_t, uint32_t>& old_to_new,
                            std::vector<uint32_t>* type_reordering) {
  always_assert_log(type_reordering->empty(),
                    "Expected to fill empty output vec");
  for (uint32_t i = 0; i < entry_count; i++) {
    type_reordering->emplace_back(i);
  }
  bool has_change = false;
  for (const auto& pair : old_to_new) {
    uint32_t p = (pair.first & PACKAGE_MASK_BIT) >> PACKAGE_INDEX_BIT_SHIFT;
    uint8_t t = (pair.first & TYPE_MASK_BIT) >> TYPE_INDEX_BIT_SHIFT;
    if (p == package_id && t == type_id) {
      uint32_t old_entry = pair.first & ENTRY_MASK_BIT;
      uint32_t new_entry = pair.second & ENTRY_MASK_BIT;
      if ((*type_reordering)[new_entry] != old_entry) {
        has_change = true;
        (*type_reordering)[new_entry] = old_entry;
      }
    }
  }
  return has_change;
}
} // namespace

void ResourcesArscFile::remap_reorder_and_serialize(
    const std::vector<std::string>& /* resource_files */,
    const std::map<uint32_t, uint32_t>& old_to_new) {
  remap_ids(old_to_new);
  apk::TableEntryParser table_parser;
  table_parser.visit(m_f.data(), m_arsc_len);
  arsc::ResTableBuilder table_builder;
  table_builder.set_global_strings(table_parser.m_global_pool_header);
  for (auto& package : table_parser.m_packages) {
    auto package_id = dtohl(package->id);
    auto package_builder = std::make_shared<arsc::ResPackageBuilder>(package);
    package_builder->set_key_strings(
        table_parser.m_package_key_string_headers.at(package));
    package_builder->set_type_strings(
        table_parser.m_package_type_string_headers.at(package));
    auto& type_infos = table_parser.m_package_types.at(package);
    for (auto& type_info : type_infos) {
      // Check if this type needs re-ordering. If so, rebuild it via the
      // ResTableTypeDefiner. Otherwise, copy the TypeInfo as-is.
      uint8_t type_id = type_info.spec->id;
      auto entry_count = dtohl(type_info.spec->entryCount);
      std::vector<uint32_t> type_entries_new_to_old;
      type_entries_new_to_old.reserve(entry_count);
      if (create_type_reordering(package_id, type_id, entry_count, old_to_new,
                                 &type_entries_new_to_old)) {
        TRACE(RES, 9, "Type ID 0x%x will be rebuilt with new order", type_id);
        std::vector<uint32_t> flags;
        flags.reserve(entry_count);
        // Set up the new ordering of the flags, based on the projected
        // reordering.
        for (uint32_t new_entry_id = 0; new_entry_id < entry_count;
             new_entry_id++) {
          uint32_t old_id = MAKE_RES_ID(package_id, type_id,
                                        type_entries_new_to_old[new_entry_id]);
          flags.emplace_back(table_parser.m_res_id_to_flags.at(old_id));
        }

        auto configs = table_parser.get_configs(package_id, type_id);
        auto type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
            package_id, type_id, configs, flags);

        for (auto& config : configs) {
          for (uint32_t new_entry_id = 0; new_entry_id < entry_count;
               new_entry_id++) {
            uint32_t old_id = MAKE_RES_ID(
                package_id, type_id, type_entries_new_to_old[new_entry_id]);
            auto ev = table_parser.get_entry_for_config(old_id, config);
            type_definer->add(config, ev);
          }
        }
        package_builder->add_type(type_definer);
      } else {
        TRACE(RES, 9, "No ordering change for type ID 0x%x", type_id);
        package_builder->add_type(type_info);
      }
    }
    // Copy unknown chunks that we did not parse
    auto& unknown_chunks = table_parser.m_package_unknown_chunks.at(package);
    for (auto& header : unknown_chunks) {
      package_builder->add_chunk(header);
    }
    table_builder.add_package(package_builder);
  }
  android::Vector<char> out;
  table_builder.serialize(&out);
  m_arsc_len = write_serialized_data(out, std::move(m_f));
  m_file_closed = true;
}

std::unordered_set<std::string> ResourcesArscFile::get_files_by_rid(
    uint32_t res_id, ResourcePathType /* unused */) {
  std::unordered_set<std::string> ret;
  android::Vector<android::Res_value> out_values;
  res_table.getAllValuesForResource(res_id, out_values);
  for (size_t i = 0; i < out_values.size(); i++) {
    auto val = out_values[i];
    if (val.dataType == android::Res_value::TYPE_STRING) {
      // data is an index into string pool.
      auto file_path = res_table.getString8FromIndex(0, val.data);
      auto file_chars = file_path.string();
      if (file_chars != nullptr) {
        auto file_str = std::string(file_chars);
        if (is_resource_file(file_str)) {
          ret.emplace(file_str);
        }
      }
    }
  }
  return ret;
}

void ResourcesArscFile::walk_references_for_resource(
    uint32_t resID,
    std::unordered_set<uint32_t>* nodes_visited,
    std::unordered_set<std::string>* potential_file_paths) {
  if (nodes_visited->find(resID) != nodes_visited->end()) {
    return;
  }
  nodes_visited->emplace(resID);

  ssize_t pkg_index = res_table.getResourcePackageIndex(resID);

  android::Vector<android::Res_value> initial_values;
  res_table.getAllValuesForResource(resID, initial_values);

  std::stack<android::Res_value> nodes_to_explore;
  for (size_t index = 0; index < initial_values.size(); ++index) {
    nodes_to_explore.push(initial_values[index]);
  }

  while (!nodes_to_explore.empty()) {
    android::Res_value r = nodes_to_explore.top();
    nodes_to_explore.pop();
    if (r.dataType == android::Res_value::TYPE_STRING) {
      android::String8 str = res_table.getString8FromIndex(pkg_index, r.data);
      potential_file_paths->insert(std::string(str.string()));
      continue;
    }

    // Skip any non-references or already visited nodes
    if ((r.dataType != android::Res_value::TYPE_REFERENCE &&
         r.dataType != android::Res_value::TYPE_ATTRIBUTE) ||
        r.data <= PACKAGE_RESID_START ||
        nodes_visited->find(r.data) != nodes_visited->end()) {
      continue;
    }

    nodes_visited->insert(r.data);
    android::Vector<android::Res_value> inner_values;
    res_table.getAllValuesForResource(r.data, inner_values);
    for (size_t index = 0; index < inner_values.size(); ++index) {
      nodes_to_explore.push(inner_values[index]);
    }
  }
}

void ResourcesArscFile::delete_resource(uint32_t res_id) {
  res_table.deleteResource(res_id);
}

void ResourcesArscFile::collect_resid_values_and_hashes(
    const std::vector<uint32_t>& ids,
    std::map<size_t, std::vector<uint32_t>>* res_by_hash) {
  tmp_id_to_values.clear();
  for (uint32_t id : ids) {
    android::Vector<android::Res_value> row_values;
    res_table.getAllValuesForResource(id, row_values);
    (*res_by_hash)[getHashFromValues(row_values)].push_back(id);
    tmp_id_to_values[id] = row_values;
  }
}

bool ResourcesArscFile::resource_value_identical(uint32_t a_id, uint32_t b_id) {
  if (tmp_id_to_values[a_id].size() != tmp_id_to_values[b_id].size()) {
    return false;
  }

  return res_table.areResourceValuesIdentical(a_id, b_id);
}

ResourcesArscFile::ResourcesArscFile(const std::string& path)
    : m_f(RedexMappedFile::open(path, /* read_only= */ false)) {
  m_arsc_len = m_f.size();
  int error = res_table.add(m_f.const_data(), m_f.size(), /* cookie */ -1,
                            /* copyData*/ true);
  always_assert_log(error == 0, "Reading arsc failed with error code: %d",
                    error);

  res_table.getResourceIds(&sorted_res_ids);

  // Build up maps to/from resource ID's and names
  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    uint32_t id = sorted_res_ids[index];
    android::ResTable::resource_name name;
    res_table.getResourceName(id, true, &name);
    std::string name_string(
        android::String8(name.name8, name.nameLen).string());
    id_to_name.emplace(id, name_string);
    name_to_ids[name_string].push_back(id);
  }
}

size_t ResourcesArscFile::get_length() const { return m_arsc_len; }

size_t ResourcesArscFile::serialize() {
  android::Vector<char> cVec;
  res_table.serialize(cVec, 0);
  m_arsc_len = write_serialized_data(cVec, std::move(m_f));
  m_file_closed = true;
  return m_arsc_len;
}

void ResourcesArscFile::remap_ids(
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) {
  android::SortedVector<uint32_t> old;
  android::Vector<uint32_t> remapped;
  for (const auto& pair : old_to_remapped_ids) {
    old.add(pair.first);
    remapped.add(pair.second);
  }

  for (const auto& pair : old_to_remapped_ids) {
    res_table.remapReferenceValuesForResource(pair.first, old, remapped);
  }
}

std::unordered_set<uint32_t> ResourcesArscFile::get_types_by_name(
    const std::unordered_set<std::string>& type_names) {

  android::Vector<android::String8> typeNames;
  res_table.getTypeNamesForPackage(0, &typeNames);

  std::unordered_set<uint32_t> type_ids;
  for (size_t i = 0; i < typeNames.size(); ++i) {
    std::string typeStr(typeNames[i].string());
    if (type_names.count(typeStr) == 1) {
      type_ids.emplace((i + 1) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

std::unordered_set<uint32_t> ResourcesArscFile::get_types_by_name_prefixes(
    const std::unordered_set<std::string>& type_name_prefixes) {

  android::Vector<android::String8> typeNames;
  res_table.getTypeNamesForPackage(0, &typeNames);

  std::unordered_set<uint32_t> type_ids;
  for (size_t i = 0; i < typeNames.size(); ++i) {
    std::string typeStr(typeNames[i].string());
    if (std::find_if(type_name_prefixes.begin(), type_name_prefixes.end(),
                     [&](const std::string& prefix) {
                       return typeStr.find(prefix) != std::string::npos;
                     }) != type_name_prefixes.end()) {
      type_ids.emplace((i + 1) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

std::vector<std::string> ResourcesArscFile::get_resource_strings_by_name(
    const std::string& res_name) {
  std::vector<std::string> ret;
  auto it = name_to_ids.find(res_name);
  if (it != name_to_ids.end()) {
    ret.reserve(it->second.size());
    for (uint32_t id : it->second) {
      android::Res_value res_value;
      res_table.getResource(id, &res_value);

      // just in case there's a reference
      res_table.resolveReference(&res_value, 0);
      size_t len = 0;

      // aapt is using 0, so why not?
      const char16_t* str =
          res_table.getTableStringBlock(0)->stringAt(res_value.data, &len);
      if (str) {
        ret.push_back(android::String8(str, len).string());
      }
    }
  }
  return ret;
}

ResourcesArscFile::~ResourcesArscFile() {}
