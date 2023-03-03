/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include <fstream>
#include <map>
#include <string_view>

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
#include "utils/Unicode.h"
#include "utils/Visitor.h"

#include "Debug.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
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
bool is_valid_string_idx(const android::ResStringPool& pool, size_t idx) {
  size_t u16_len;
  return pool.stringAt(idx, &u16_len) != nullptr;
}

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

bool XmlFileEditor::visit_global_strings(android::ResStringPool_header* pool) {
  m_string_pool_header = pool;
  return true;
}

bool XmlFileEditor::visit_attribute_ids(uint32_t* id, size_t count) {
  arsc::XmlFileVisitor::visit_attribute_ids(id, count);
  m_attribute_ids_start = id;
  m_attribute_id_count = count;
  return true;
}

bool XmlFileEditor::visit_typed_data(android::Res_value* value) {
  m_typed_data.emplace_back(value);
  return true;
}

size_t XmlFileEditor::remap(const std::map<uint32_t, uint32_t>& old_to_new) {
  size_t changes = 0;
  if (m_attribute_id_count > 0 && m_attribute_ids_start != nullptr) {
    uint32_t* id = m_attribute_ids_start;
    for (size_t i = 0; i < m_attribute_id_count; i++, id++) {
      uint32_t old_value = dtohl(*id);
      auto search = old_to_new.find(old_value);
      if (search != old_to_new.end()) {
        auto new_value = htodl(search->second);
        if (old_value != new_value) {
          TRACE(RES, 9, "Remapping attribute ID 0x%x -> 0x%x at offset %ld",
                old_value, new_value, get_file_offset(id));
          *id = new_value;
          changes++;
        }
      }
    }
  }
  for (auto& value : m_typed_data) {
    if (value->dataType == android::Res_value::TYPE_REFERENCE ||
        value->dataType == android::Res_value::TYPE_ATTRIBUTE) {
      auto old_value = dtohl(value->data);
      auto search = old_to_new.find(old_value);
      if (search != old_to_new.end()) {
        auto new_value = htodl(search->second);
        if (old_value != new_value) {
          TRACE(RES, 9, "Remapping attribute value 0x%x -> 0x%x at offset %ld",
                old_value, new_value, get_file_offset(value));
          value->data = new_value;
          changes++;
        }
      }
    }
  }
  return changes;
}

#define GET_ID(ptr) (dtohl((ptr)->id))
#define CHUNK_SIZE(chunk) (dtohl((chunk)->header.size))

TableSnapshot::TableSnapshot(RedexMappedFile& mapped_file, size_t len) {
  auto data = mapped_file.read_only ? (void*)mapped_file.const_data()
                                    : mapped_file.data();
  always_assert_log(m_table_parser.visit(data, len),
                    "Failed to parse .arsc file");
  always_assert_log(
      m_global_strings.setTo(m_table_parser.m_global_pool_header,
                             CHUNK_SIZE(m_table_parser.m_global_pool_header),
                             true) == android::NO_ERROR,
      "Failed to parse global strings!");
  for (const auto& pair : m_table_parser.m_package_key_string_headers) {
    auto package_id = GET_ID(pair.first);
    always_assert_log(
        m_key_strings[package_id].setTo(pair.second, CHUNK_SIZE(pair.second),
                                        true) == android::NO_ERROR,
        "Failed to parse key strings for package 0x%x", package_id);
  }
  for (const auto& pair : m_table_parser.m_package_type_string_headers) {
    auto package_id = GET_ID(pair.first);
    always_assert_log(
        m_type_strings[package_id].setTo(pair.second, CHUNK_SIZE(pair.second),
                                         true) == android::NO_ERROR,
        "Failed to parse type strings for package 0x%x", package_id);
  }
}

void TableSnapshot::gather_non_empty_resource_ids(std::vector<uint32_t>* ids) {
  for (const auto& pair : m_table_parser.m_res_id_to_entries) {
    auto id = pair.first;
    for (const auto& config_entry : pair.second) {
      if (!arsc::is_empty(config_entry.second)) {
        ids->emplace_back(id);
        break;
      }
    }
  }
}

std::string TableSnapshot::get_resource_name(uint32_t id) {
  uint32_t package_id = (id >> PACKAGE_INDEX_BIT_SHIFT) & 0xFF;
  auto search = m_table_parser.m_res_id_to_entries.find(id);
  if (search == m_table_parser.m_res_id_to_entries.end()) {
    return "";
  }
  auto& entries = search->second;
  ssize_t result = -1;
  for (const auto& pair : entries) {
    auto entry_data = pair.second.getKey();
    if (entry_data != nullptr) {
      auto entry = (android::ResTable_entry*)entry_data;
      auto index = dtohl(entry->key.index);
      always_assert_log(result < 0 || result == index,
                        "Malformed entry data for ID 0x%x", id);
      result = index;
    }
  }
  if (result < 0) {
    return "";
  }
  auto& pool = m_key_strings.at(package_id);
  return get_string_from_pool(pool, result);
}

size_t TableSnapshot::package_count() {
  return m_table_parser.m_packages.size();
}

void TableSnapshot::get_type_names(uint32_t package_id,
                                   std::vector<std::string>* out) {
  auto& pool = m_type_strings.at(package_id);
  for (size_t i = 0; i < pool.size(); i++) {
    out->emplace_back(get_string_from_pool(pool, i));
  }
}

void TableSnapshot::get_configurations(
    uint32_t package_id,
    const std::string& type_name,
    std::vector<android::ResTable_config>* out) {
  uint8_t type_id = 0;
  auto& pool = m_type_strings.at(package_id);
  for (size_t i = 0; i < pool.size(); i++) {
    if (type_name == get_string_from_pool(pool, i)) {
      type_id = i + 1;
      break;
    }
  }
  if (type_id > 0) {
    auto configs = m_table_parser.get_configs(package_id, type_id);
    for (const auto& c : configs) {
      always_assert_log(sizeof(android::ResTable_config) >= dtohl(c->size),
                        "Config at %p has unexpected size %d", c,
                        dtohl(c->size));
      android::ResTable_config swapped;
      swapped.copyFromDtoH(*c);
      out->emplace_back(swapped);
    }
  }
}

std::set<android::ResTable_config> TableSnapshot::get_configs_with_values(
    uint32_t id) {
  std::set<android::ResTable_config> configs;
  auto& config_entries = m_table_parser.m_res_id_to_entries.at(id);
  for (auto& pair : config_entries) {
    if (!arsc::is_empty(pair.second)) {
      android::ResTable_config swapped;
      swapped.copyFromDtoH(*(pair.first));
      configs.emplace(swapped);
    }
  }
  return configs;
}

bool TableSnapshot::are_values_identical(uint32_t a, uint32_t b) {
  uint32_t upper = PACKAGE_MASK_BIT | TYPE_MASK_BIT;
  if ((a & upper) != (b & upper)) {
    return false;
  }
  if (m_table_parser.m_res_id_to_flags.at(a) !=
      m_table_parser.m_res_id_to_flags.at(b)) {
    return false;
  }
  auto a_entries = m_table_parser.m_res_id_to_entries.at(a);
  auto b_entries = m_table_parser.m_res_id_to_entries.at(b);
  always_assert_log(a_entries.size() == b_entries.size(),
                    "Parsed table representation does not have the same set of "
                    "configs for ids 0x%x, 0x%x",
                    a, b);
  for (auto& pair : a_entries) {
    auto& a_entry = pair.second;
    auto& b_entry = b_entries.at(pair.first);
    auto empty_entry = arsc::is_empty(a_entry);
    if (empty_entry != arsc::is_empty(b_entry)) {
      return false;
    }
    // Total length of entry + value data
    if (a_entry.getValue() != b_entry.getValue()) {
      return false;
    }
    if (!empty_entry) {
      auto a_data = arsc::get_value_data(a_entry);
      auto b_data = arsc::get_value_data(b_entry);
      auto empty_data = arsc::is_empty(a_data);
      auto data_len = a_data.getValue();
      if (data_len != b_data.getValue()) {
        return false;
      }
      if (empty_data != arsc::is_empty(b_data)) {
        return false;
      }
      if (!empty_data &&
          memcmp(a_data.getKey(), b_data.getKey(), data_len) != 0) {
        return false;
      }
    }
  }
  return true;
}

namespace {
// Parse the entry data (which could be in many forms) and emit a flattened list
// of Res_value objects for callers to easily consume (by legacy convention,
// this will do byte swapping).
class EntryFlattener : public arsc::ResourceTableVisitor {
 public:
  void emit(android::Res_value* source) {
    android::Res_value value;
    value.size = dtohs(source->size);
    value.res0 = source->res0;
    value.dataType = source->dataType;
    value.data = dtohl(source->data);
    m_values.emplace_back(value);
  }

  void emit_shim(uint8_t data_type, uint32_t data) {
    android::Res_value shim;
    shim.dataType = data_type;
    shim.data = data;
    m_values.emplace_back(shim);
  }

  bool visit_entry(android::ResTable_package* /* unused */,
                   android::ResTable_typeSpec* /* unused */,
                   android::ResTable_type* /* unused */,
                   android::ResTable_entry* /* unused */,
                   android::Res_value* value) override {
    emit(value);
    return true;
  }

  bool visit_map_entry(android::ResTable_package* /* unused */,
                       android::ResTable_typeSpec* /* unused */,
                       android::ResTable_type* /* unused */,
                       android::ResTable_map_entry* entry) override {
    // API QUIRK: Old code that served this purpose left size intentionally as 0
    // to denote this is a "conceptual" value that is encompassed by the
    // resource ID we are being asked to traverse. This is useful for
    // reachability purposes but a bit misleading otherwise.
    emit_shim(android::Res_value::TYPE_REFERENCE, dtohl(entry->parent.ident));
    return true;
  }

  bool visit_map_value(android::ResTable_package* /* unused */,
                       android::ResTable_typeSpec* /* unused */,
                       android::ResTable_type* /* unused */,
                       android::ResTable_map_entry* /* unused */,
                       android::ResTable_map* value) override {
    emit(&value->value);
    // API QUIRK: Same rationale as above.
    emit_shim(android::Res_value::TYPE_ATTRIBUTE, dtohl(value->name.ident));
    return true;
  }

  std::vector<android::Res_value> m_values;
};
} // namespace

void TableSnapshot::collect_resource_values(
    uint32_t id, std::vector<android::Res_value>* out) {
  collect_resource_values(id, {}, out);
}

void TableSnapshot::collect_resource_values(
    uint32_t id,
    std::vector<android::ResTable_config> include_configs,
    std::vector<android::Res_value>* out) {
  auto should_include_config = [&](android::ResTable_config* maybe) {
    if (include_configs.empty()) {
      return true;
    }
    for (auto& c : include_configs) {
      if (arsc::are_configs_equivalent(maybe, &c)) {
        return true;
      }
    }
    return false;
  };
  auto& config_entries = m_table_parser.m_res_id_to_entries.at(id);
  for (auto& pair : config_entries) {
    if (should_include_config(pair.first)) {
      auto ev = pair.second;
      auto entry = (android::ResTable_entry*)ev.getKey();
      EntryFlattener flattener;
      flattener.begin_visit_entry(nullptr, nullptr, nullptr, entry);
      for (auto& v : flattener.m_values) {
        out->push_back(v);
      }
    }
  }
}

bool TableSnapshot::is_valid_global_string_idx(size_t idx) const {
  return is_valid_string_idx(m_global_strings, idx);
}

std::string TableSnapshot::get_global_string(size_t idx) const {
  return get_string_from_pool(m_global_strings, idx);
}

} // namespace apk

namespace {

#define MAKE_RES_ID(package, type, entry)                        \
  ((PACKAGE_MASK_BIT & ((package) << PACKAGE_INDEX_BIT_SHIFT)) | \
   (TYPE_MASK_BIT & ((type) << TYPE_INDEX_BIT_SHIFT)) |          \
   (ENTRY_MASK_BIT & (entry)))

/**
 * write_serialized_data don't support growing arsc file, so use
 * ofstream to write to arsc file for input bigger than original
 * file size.
 */
size_t write_serialized_data_with_expansion(const android::Vector<char>& cVec,
                                            RedexMappedFile f) {
  size_t vec_size = cVec.size();
  auto filename = f.filename.c_str();
  // Close current opened file.
  f.file.reset();
  // Write to arsc through ofstream
  std::ofstream ofs(filename,
                    std::ofstream::out | std::ofstream::trunc |
                        std::ofstream::binary);
  ofs.write(&(cVec[0]), vec_size);
  return vec_size;
}

size_t write_serialized_data(const android::Vector<char>& cVec,
                             RedexMappedFile f) {
  size_t vec_size = cVec.size();
  size_t f_size = f.size();
  always_assert_log(vec_size <= f_size, "Growing file not supported");
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
 * skip a tag including its nested tags
 */
void skip_nested_tags(android::ResXMLTree* parser) {
  size_t depth{1};
  while (depth) {
    auto type = parser->next();
    switch (type) {
    case android::ResXMLParser::START_TAG: {
      ++depth;
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
  android::String16 queries("queries");
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
      } else if (tag == queries) {
        // queries break the logic to find providers below because they don't
        // declare a name
        skip_nested_tags(&parser);
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
  auto chunk = (android::ResChunk_header*)data;
  return dtohs(chunk->type) == android::RES_XML_TYPE &&
         dtohs(chunk->headerSize) == sizeof(android::ResChunk_header) &&
         dtohl(chunk->size) == size;
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

namespace {
class XmlStringAttributeCollector : public arsc::XmlFileVisitor {
 public:
  ~XmlStringAttributeCollector() override {}

  bool visit_global_strings(android::ResStringPool_header* pool) override {
    always_assert_log(m_string_pool->setTo(pool, dtohl(pool->header.size),
                                           true) == android::NO_ERROR,
                      "Failed to parse xml strings!");
    return true;
  }

  bool visit_typed_data(android::Res_value* value) override {
    if (value->dataType == android::Res_value::TYPE_STRING) {
      auto idx = dtohl(value->data);
      if (apk::is_valid_string_idx(*m_string_pool, idx)) {
        auto s = apk::get_string_from_pool(*m_string_pool, idx);
        m_values.emplace(s);
      }
    }
    return true;
  }

  std::shared_ptr<android::ResStringPool> m_string_pool =
      std::make_shared<android::ResStringPool>();
  std::unordered_set<std::string> m_values;
};
} // namespace

void ApkResources::collect_xml_attribute_string_values_for_file(
    const std::string& file_path, std::unordered_set<std::string>* out) {
  redex::read_file_with_contents(file_path, [&](const char* data, size_t size) {
    if (is_binary_xml(data, size)) {
      XmlStringAttributeCollector collector;
      if (collector.visit((void*)data, size)) {
        out->insert(collector.m_values.begin(), collector.m_values.end());
      }
    }
  });
}

namespace {
template <typename ValueType>
boost::optional<ValueType> read_xml_value(
    const std::string& file_path,
    const std::string& tag_name,
    const uint8_t& data_type,
    const std::string& attribute,
    const std::function<boost::optional<ValueType>(android::ResXMLTree&,
                                                   size_t)>& attribute_reader) {
  if (!boost::filesystem::exists(file_path)) {
    return boost::none;
  }

  auto file = RedexMappedFile::open(file_path);

  if (file.size() == 0) {
    fprintf(stderr, "WARNING: Cannot find/read the xml file %s\n",
            file_path.c_str());
    return boost::none;
  }

  android::ResXMLTree parser;
  parser.setTo(file.const_data(), file.size());

  if (parser.getError() != android::NO_ERROR) {
    fprintf(stderr, "WARNING: Failed to parse the xml file %s\n",
            file_path.c_str());
    return boost::none;
  }

  const android::String16 tag(tag_name.c_str());
  const android::String16 attr(attribute.c_str());
  android::ResXMLParser::event_code_t event_code;
  do {
    event_code = parser.next();
    if (event_code == android::ResXMLParser::START_TAG) {
      size_t outLen;
      auto el_name = android::String16(parser.getElementName(&outLen));
      if (el_name == tag) {
        const size_t attr_count = parser.getAttributeCount();
        for (size_t i = 0; i < attr_count; ++i) {
          size_t len;
          android::String16 key(parser.getAttributeName(i, &len));
          if (key == attr && parser.getAttributeDataType(i) == data_type) {
            return attribute_reader(parser, i);
          }
        }
      }
    }
  } while ((event_code != android::ResXMLParser::END_DOCUMENT) &&
           (event_code != android::ResXMLParser::BAD_DOCUMENT));
  return boost::none;
}
} // namespace

boost::optional<int32_t> ApkResources::get_min_sdk() {
  return read_xml_value<int32_t>(
      m_manifest, "uses-sdk", android::Res_value::TYPE_INT_DEC, "minSdkVersion",
      [](android::ResXMLTree& parser, size_t idx) {
        return boost::optional<int32_t>(
            static_cast<int32_t>(parser.getAttributeData(idx)));
      });
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

boost::optional<std::string> ApkResources::get_manifest_package_name() {
  return read_xml_value<std::string>(
      m_manifest, "manifest", android::Res_value::TYPE_STRING, "package",
      [](android::ResXMLTree& parser, size_t idx) {
        size_t len;
        auto chars = parser.getAttributeStringValue(idx, &len);
        if (chars == nullptr) {
          return boost::optional<std::string>{};
        }
        android::String16 s16(chars, len);
        return boost::optional<std::string>(convert_from_string16(s16));
      });
}

std::unordered_set<uint32_t> ApkResources::get_xml_reference_attributes(
    const std::string& filename) {
  if (is_raw_resource(filename)) {
    return {};
  }
  auto file = RedexMappedFile::open(filename);
  apk::XmlValueCollector collector;
  collector.visit((void*)file.const_data(), file.size());
  return collector.m_ids;
}

namespace {
// Insert string data from the given pool at the given index to the builder.
void add_existing_string_to_builder(const android::ResStringPool& string_pool,
                                    arsc::ResStringPoolBuilder* builder,
                                    size_t idx) {
  size_t length;
  if (string_pool.isUTF8()) {
    auto s = string_pool.string8At(idx, &length);
    builder->add_string(s, length);
  } else {
    auto s = string_pool.stringAt(idx, &length);
    builder->add_string(s, length);
  }
}
} // namespace

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
  if (!is_binary_xml(data, len)) {
    return android::BAD_TYPE;
  }
  auto pool_ptr = (android::ResStringPool_header*)((char*)data + chunk_size);
  if (dtohs(pool_ptr->header.type) != android::RES_STRING_POOL_TYPE) {
    return android::BAD_TYPE;
  }

  size_t num_replaced = 0;
  android::ResStringPool pool(pool_ptr, dtohl(pool_ptr->header.size));
  auto flags = pool.isUTF8() ? htodl(android::ResStringPool_header::UTF8_FLAG)
                             : (uint32_t)0;
  arsc::ResStringPoolBuilder pool_builder(flags);
  for (size_t i = 0; i < dtohl(pool_ptr->stringCount); i++) {
    auto existing_str = apk::get_string_from_pool(pool, i);
    auto replacement = rename_map.find(existing_str);
    if (replacement == rename_map.end()) {
      add_existing_string_to_builder(pool, &pool_builder, i);
    } else {
      pool_builder.add_string(replacement->second);
      num_replaced++;
    }
  }

  *out_num_renamed = num_replaced;
  if (num_replaced > 0) {
    arsc::replace_xml_string_pool((android::ResChunk_header*)data, len,
                                  pool_builder, out_data);
  }
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
size_t getHashFromValues(const std::vector<android::Res_value>& values) {
  size_t hash = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    boost::hash_combine(hash, values[i].data);
  }
  return hash;
}

} // namespace

size_t ApkResources::remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) {
  if (is_raw_resource(filename)) {
    return 0;
  }
  auto file = RedexMappedFile::open(filename, false);
  apk::XmlFileEditor editor;
  always_assert_log(editor.visit((void*)file.data(), file.size()),
                    "Failed to parse resource xml file %s", filename.c_str());
  return editor.remap(kept_to_remapped_ids) > 0;
}

namespace {

void obfuscate_xml_attributes(
    const std::string& filename,
    const std::unordered_set<std::string>& do_not_obfuscate_elements) {
  auto file = RedexMappedFile::open(filename, false);
  apk::XmlFileEditor editor;
  always_assert_log(editor.visit((void*)file.data(), file.size()),
                    "Failed to parse resource xml file %s", filename.c_str());
  auto attribute_count = editor.m_attribute_id_count;
  auto string_count = dtohl(editor.m_string_pool_header->stringCount);
  if (attribute_count > 0 && string_count > 0) {
    TRACE(RES, 9, "Considering file %s for obfuscation", filename.c_str());
    android::ResStringPool pool;
    always_assert_log(
        pool.setTo(editor.m_string_pool_header,
                   dtohl(editor.m_string_pool_header->header.size),
                   true) == android::NO_ERROR,
        "Malformed string pool in file %s", filename.c_str());
    uint32_t flags =
        pool.isUTF8() ? android::ResStringPool_header::UTF8_FLAG : 0;
    android::String8 empty_str("");
    arsc::ResStringPoolBuilder builder(flags);
    for (size_t i = 0; i < attribute_count; i++) {
      builder.add_string(empty_str.string(), empty_str.size());
    }
    for (size_t i = attribute_count; i < string_count; i++) {
      auto element = apk::get_string_from_pool(pool, i);
      if (do_not_obfuscate_elements.count(element) > 0) {
        TRACE(RES, 9, "NOT obfuscating xml file %s", filename.c_str());
        return;
      }
      builder.add_string(element);
    }
    android::Vector<char> out;
    arsc::replace_xml_string_pool((android::ResChunk_header*)file.data(),
                                  file.size(), builder, &out);
    write_serialized_data(out, std::move(file));
  }
}
} // namespace

void ApkResources::obfuscate_xml_files(
    const std::unordered_set<std::string>& allowed_types,
    const std::unordered_set<std::string>& do_not_obfuscate_elements) {
  using path_t = boost::filesystem::path;
  using dir_iterator = boost::filesystem::directory_iterator;

  std::set<std::string> xml_paths;
  path_t res(m_directory + "/res");
  if (exists(res) && is_directory(res)) {
    for (auto it = dir_iterator(res); it != dir_iterator(); ++it) {
      auto const& entry = *it;
      const path_t& entry_path = entry.path();
      const auto& entry_string = entry_path.string();
      // TODO(T126661220): support obfuscated input.
      if (is_directory(entry_path) &&
          can_obfuscate_xml_file(allowed_types, entry_string)) {
        for (const std::string& layout : get_xml_files(entry_string)) {
          xml_paths.emplace(layout);
        }
      }
    }
  }
  for (const auto& path : xml_paths) {
    obfuscate_xml_attributes(path, do_not_obfuscate_elements);
  }
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

void ResourcesArscFile::nullify_res_ids_and_serialize(
    const std::vector<std::string>& /* resource_files */) {
  m_nullify_removed = true;
  serialize();
}

namespace {
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
  apk::TableEntryParser table_parser = get_table_snapshot().get_parsed_table();
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
            package_id, type_id, configs, flags, false,
            arsc::any_sparse_types(type_info.configs));

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
  mark_file_closed();
}

namespace {
// Parses the global string pool for the resources.arsc file. Stores a lookup
// from string to the index in the pool. "Global string pool" in this case will
// refer to all string values, file paths, and styles (that is, HTML tags) used
// throughout the resource table itself.
//
// The global string pool however does NOT include:
// 1) Names of resource types, i.e. "anim", "color", "drawable", "mipmap" etc.
// 2) The keys of resource items, i.e. "app_name" for usage "@string/app_name".
// 3) Any string used in an XML document like the manifest or layout. That will
//    be held in the XML document's own string pool.
class GlobalStringPoolReader : public arsc::ResourceTableVisitor {
 public:
  ~GlobalStringPoolReader() override {}

  bool visit_global_strings(android::ResStringPool_header* header) override {
    always_assert_log(
        m_global_strings->setTo(header, dtohl(header->header.size), true) ==
            android::NO_ERROR,
        "Failed to parse global strings!");
    m_global_strings_header = header;
    auto size = m_global_strings->size();
    for (uint32_t i = 0; i < size; i++) {
      auto value = apk::get_string_from_pool(*m_global_strings, i);
      m_string_to_idx.emplace(value, i);
      TRACE(RES, 9, "GLOBAL STRING [%u] = %s", i, value.c_str());
    }
    return false; // Don't parse anything else
  }

  uint32_t get_string_idx(const std::string& s) {
    return m_string_to_idx.at(s);
  }

  std::shared_ptr<android::ResStringPool> global_strings() {
    return m_global_strings;
  }

  android::ResStringPool_header* global_string_header() {
    return m_global_strings_header;
  }

 private:
  std::shared_ptr<android::ResStringPool> m_global_strings =
      std::make_shared<android::ResStringPool>();
  android::ResStringPool_header* m_global_strings_header = nullptr;
  std::unordered_map<std::string, uint32_t> m_string_to_idx;
};

class StringPoolRefRemappingVisitor : public arsc::StringPoolRefVisitor {
 public:
  ~StringPoolRefRemappingVisitor() override {}

  explicit StringPoolRefRemappingVisitor(
      const std::unordered_map<uint32_t, uint32_t>& old_to_new)
      : m_old_to_new(old_to_new) {}

  void remap_impl(const uint32_t& idx,
                  const std::function<void(const uint32_t&)>& setter) {
    auto search = m_old_to_new.find(idx);
    if (search != m_old_to_new.end()) {
      auto new_value = search->second;
      TRACE(RES, 9, "REMAP IDX %u -> %u", idx, new_value);
      setter(htodl(new_value));
    }
  }

  bool visit_global_strings_ref(android::Res_value* value) override {
    TRACE(RES, 9, "visit string Res_value, offset = %ld",
          get_file_offset(value));
    remap_impl(dtohl(value->data),
               [&value](const uint32_t new_value) { value->data = new_value; });
    return true;
  }

 private:
  const std::unordered_map<uint32_t, uint32_t>& m_old_to_new;
};

// Collects string references into the global string pool from values and styles
// and per package, the entries into the key string pool.
class PackageStringRefCollector : public apk::TableParser {
 public:
  ~PackageStringRefCollector() override {}

  bool visit_package(android::ResTable_package* package) override {
    std::map<android::ResTable_type*, std::set<android::ResStringPool_ref*>>
        entries;
    m_package_entries.emplace(package, std::move(entries));
    std::shared_ptr<android::ResStringPool> key_strings =
        std::make_shared<android::ResStringPool>();
    m_package_key_strings.emplace(package, std::move(key_strings));
    apk::TableParser::visit_package(package);
    return true;
  }

  bool visit_key_strings(android::ResTable_package* package,
                         android::ResStringPool_header* pool) override {
    auto& key_strings = m_package_key_strings.at(package);
    always_assert_log(key_strings->getError() == android::NO_INIT,
                      "Key strings re-init!");
    always_assert_log(key_strings->setTo(pool, dtohl(pool->header.size),
                                         true) == android::NO_ERROR,
                      "Failed to parse key strings!");
    apk::TableParser::visit_key_strings(package, pool);
    return true;
  }

  bool visit_global_strings_ref(android::Res_value* value) override {
    m_values.emplace(value);
    return true;
  }

  bool visit_global_strings_ref(android::ResStringPool_ref* value) override {
    m_span_refs.emplace(value);
    return true;
  }

  bool visit_key_strings_ref(android::ResTable_package* package,
                             android::ResTable_type* type,
                             android::ResStringPool_ref* value) override {
    auto& entry_set = m_package_entries.at(package);
    entry_set[type].emplace(value);
    return true;
  }

  // Values that are references into the global string pool.
  std::set<android::Res_value*> m_values;
  // References into the global string pool from a ResStringPool_span.
  std::set<android::ResStringPool_ref*> m_span_refs;
  // References into the key string pool from entries;
  std::map<
      android::ResTable_package*,
      std::map<android::ResTable_type*, std::set<android::ResStringPool_ref*>>>
      m_package_entries;
  std::map<android::ResTable_package*, std::shared_ptr<android::ResStringPool>>
      m_package_key_strings;
};
} // namespace

void ResourcesArscFile::remap_file_paths_and_serialize(
    const std::vector<std::string>& /* resource_files */,
    const std::unordered_map<std::string, std::string>& old_to_new) {
  TRACE(RES, 9, "BEGIN GlobalStringPoolReader");
  GlobalStringPoolReader string_reader;
  string_reader.visit(m_f.data(), m_arsc_len);
  std::unordered_map<uint32_t, uint32_t> old_to_new_idx;
  for (auto& pair : old_to_new) {
    old_to_new_idx.emplace(string_reader.get_string_idx(pair.first),
                           string_reader.get_string_idx(pair.second));
  }
  TRACE(RES, 9, "BEGIN StringPoolRefRemappingVisitor");
  StringPoolRefRemappingVisitor remapper(old_to_new_idx);
  // Note: file is opened for writing. Visitor will in place change the data
  // (without altering any data sizes).
  remapper.visit(m_f.data(), m_arsc_len);
}

namespace {

// Copy an individual index from the pool to the builder. API here is weird due
// to this largely being identical for UTF-8 pools and UTF-16 pools, except for
// the data type and the API call to get the character pointer.
template <typename CharType>
void add_string_idx_to_builder(
    const android::ResStringPool& string_pool,
    size_t idx,
    const CharType* s,
    size_t len,
    const std::function<void(android::ResStringPool_span*)>& span_remapper,
    arsc::ResStringPoolBuilder* builder) {
  if (idx < string_pool.styleCount()) {
    arsc::SpanVector vec;
    arsc::collect_spans((android::ResStringPool_span*)string_pool.styleAt(idx),
                        &vec);
    for (auto& span : vec) {
      span_remapper(span);
    }
    builder->add_style(s, len, vec);
  } else {
    builder->add_string(s, len);
  }
}

// Copies the string data for the kept indicies from the given pool to the
// builder in the order specified. If needed, a remapper function can be run
// against the spans required by a kept index.
void rebuild_string_pool(
    const android::ResStringPool& string_pool,
    const std::unordered_map<uint32_t, uint32_t>& kept_old_to_new,
    const std::function<void(android::ResStringPool_span*)>& span_remapper,
    arsc::ResStringPoolBuilder* builder) {
  const auto is_utf8 = string_pool.isUTF8();
  const auto new_string_pool_size = kept_old_to_new.size();
  always_assert_log(new_string_pool_size <= string_pool.size(),
                    "Pool remapping is too large");

  std::vector<ssize_t> output_order(new_string_pool_size, -1);
  for (const auto& pair : kept_old_to_new) {
    always_assert_log(pair.second < new_string_pool_size,
                      "Pool remap idx out of bounds");
    always_assert_log(output_order.at(pair.second) == -1,
                      "Pool remapping is invalid");
    output_order.at(pair.second) = pair.first;
  }
  for (const auto& idx : output_order) {
    size_t length;
    if (is_utf8) {
      auto s = string_pool.string8At(idx, &length);
      add_string_idx_to_builder<char>(string_pool, idx, s, length,
                                      span_remapper, builder);
    } else {
      auto s = string_pool.stringAt(idx, &length);
      add_string_idx_to_builder<char16_t>(string_pool, idx, s, length,
                                          span_remapper, builder);
    }
  }
}

// Copies the string data for the kept indicies from the given pool to the
// builder in the order specified.
void rebuild_string_pool(
    const android::ResStringPool& string_pool,
    const std::unordered_map<uint32_t, uint32_t>& kept_old_to_new,
    arsc::ResStringPoolBuilder* builder) {
  rebuild_string_pool(
      string_pool, kept_old_to_new, [](android::ResStringPool_span*) {},
      builder);
}

// Given the kept strings, build the mapping from old -> new in the projected
// new string pool.
void project_string_mapping(
    const std::unordered_set<uint32_t>& used_strings,
    const android::ResStringPool& string_pool,
    std::unordered_map<uint32_t, uint32_t>* kept_old_to_new,
    bool sort_by_string_value = false) {
  always_assert(kept_old_to_new->empty());

  std::vector<uint32_t> used_indices(used_strings.begin(), used_strings.end());
  if (!sort_by_string_value) {
    std::sort(used_indices.begin(), used_indices.end());
  } else {
    always_assert_log(string_pool.styleCount() == 0,
                      "Sorting by string value not supported with styles");
    // AOSP implementation will perform binary search via strzcmp16; to ensure
    // proper order this will convert to UTF-16 if needed and run the same
    // comparison.
    std::vector<std::u16string_view> pool_items;
    auto size = string_pool.size();
    pool_items.reserve(size);
    for (size_t i = 0; i < size; i++) {
      size_t len;
      auto chars = string_pool.stringAt(i, &len);
      pool_items.emplace_back(chars, len);
    }
    std::sort(used_indices.begin(), used_indices.end(),
              [&](uint32_t a, uint32_t b) {
                const auto& view_a = pool_items.at(a);
                const auto& view_b = pool_items.at(b);
                return strzcmp16(view_a.data(), view_a.size(), view_b.data(),
                                 view_b.size()) < 0;
              });
  }
  for (const auto& i : used_indices) {
    auto new_index = kept_old_to_new->size();
    TRACE(RES, 9, "MAPPING %u => %zu", i, new_index);
    kept_old_to_new->emplace(i, new_index);
  }
}

#define POOL_FLAGS(pool)                                               \
  (((pool)->isUTF8() ? android::ResStringPool_header::UTF8_FLAG : 0) | \
   ((pool)->isSorted() ? android::ResStringPool_header::SORTED_FLAG : 0))

#define POOL_FLAGS_CLEAR_SORT(pool) \
  ((pool)->isUTF8() ? android::ResStringPool_header::UTF8_FLAG : 0)

void rebuild_type_strings(
    const uint32_t& package_id,
    const android::ResStringPool& string_pool,
    const std::vector<resources::TypeDefinition>& added_types,
    arsc::ResStringPoolBuilder* builder) {
  always_assert_log(string_pool.styleCount() == 0,
                    "type strings should not have styles");
  const auto original_string_count = string_pool.size();
  for (size_t idx = 0; idx < original_string_count; idx++) {
    add_existing_string_to_builder(string_pool, builder, idx);
  }
  for (auto& type_def : added_types) {
    if (type_def.package_id != package_id) {
      continue;
    }
    builder->add_string(type_def.name);
  }
}
} // namespace

void ResourcesArscFile::finalize_resource_table(const ResourceConfig& config) {
  // Find the global string pool and read its settings.
  GlobalStringPoolReader string_reader;
  string_reader.visit(m_f.data(), m_arsc_len);
  auto string_pool = string_reader.global_strings();
  TRACE(RES, 9, "Global string pool has %zu styles and %zu total strings",
        string_pool->styleCount(), string_pool->size());

  // 1) Collect all referenced global string indicies and key string indicies.
  PackageStringRefCollector collector;
  collector.visit(m_f.data(), m_arsc_len);
  std::unordered_set<uint32_t> used_global_strings;
  for (const auto& value : collector.m_values) {
    used_global_strings.emplace(dtohl(value->data));
  }
  for (const auto& value : collector.m_span_refs) {
    used_global_strings.emplace(dtohl(value->index));
  }

  // 2) Build the compacted map of old -> new indicies for used global strings.
  std::unordered_map<uint32_t, uint32_t> global_old_to_new;
  project_string_mapping(used_global_strings, *string_pool, &global_old_to_new);

  // 3) Remap all Res_value structs
  auto remap_value = [&global_old_to_new](android::Res_value* value) {
    always_assert_log(value->dataType == android::Res_value::TYPE_STRING,
                      "Wrong data type for string remapping");
    auto old = dtohl(value->data);
    TRACE(RES, 9, "REMAP OLD %u", old);
    auto remapped_data = global_old_to_new.at(old);
    value->data = htodl(remapped_data);
  };
  for (const auto& value : collector.m_values) {
    remap_value(value);
  }

  // 4) Actually build the new global ResStringPool. While doing this, remap all
  //    span refs encountered (in case ResStringPool has copied its underlying
  //    data).
  std::unordered_set<android::ResStringPool_span*> remapped_spans;
  auto remap_spans = [&global_old_to_new,
                      &remapped_spans](android::ResStringPool_span* span) {
    // Guard against span offsets that have been "canonicalized"
    if (remapped_spans.count(span) == 0) {
      remapped_spans.emplace(span);
      auto old = dtohl(span->name.index);
      TRACE(RES, 9, "REMAP OLD %u", old);
      span->name.index = htodl(global_old_to_new.at(old));
    }
  };
  std::shared_ptr<arsc::ResStringPoolBuilder> global_strings_builder =
      std::make_shared<arsc::ResStringPoolBuilder>(POOL_FLAGS(string_pool));
  rebuild_string_pool(*string_pool, global_old_to_new, remap_spans,
                      global_strings_builder.get());

  // 4) Serialize the ResTable with the modified ResStringPool (which will have
  // a different size).
  arsc::ResTableBuilder table_builder;
  table_builder.set_global_strings(global_strings_builder);
  for (auto& package_entries : collector.m_package_entries) {
    // 5) Do a similar remapping as above, but for key strings.
    auto& package = package_entries.first;
    std::shared_ptr<arsc::ResPackageBuilder> package_builder =
        std::make_shared<arsc::ResPackageBuilder>(package);

    // Build new key string pool indicies.
    std::set<android::ResStringPool_ref*> refs;
    for (const auto& package_entry_pairs : package_entries.second) {
      const auto& package_type_entries = package_entry_pairs.second;
      refs.insert(package_type_entries.begin(), package_type_entries.end());
    }
    auto key_string_pool = collector.m_package_key_strings.at(package);
    std::unordered_set<uint32_t> used_key_strings;
    for (auto& ref : refs) {
      used_key_strings.emplace(dtohl(ref->index));
    }
    std::unordered_map<uint32_t, uint32_t> key_old_to_new;
    project_string_mapping(used_key_strings, *key_string_pool, &key_old_to_new,
                           config.sort_key_strings);

    auto& type_strings_header =
        collector.m_package_type_string_headers.at(package);
    android::ResStringPool type_strings;
    always_assert_log(type_strings.setTo(type_strings_header,
                                         CHUNK_SIZE(type_strings_header),
                                         true) == android::NO_ERROR,
                      "Failed to parse type strings!");
    std::vector<std::string> kept_type_names(type_strings.size());
    int last_kept_type_name = 0;

    // Remap the entries.
    for (auto& ref : refs) {
      auto old = dtohl(ref->index);
      TRACE(RES, 9, "REMAP OLD KEY %u", old);
      ref->index = htodl(key_old_to_new.at(old));
    }

    // Actually build the key strings pool.
    auto key_flags = POOL_FLAGS(key_string_pool);
    if (config.sort_key_strings) {
      key_flags |= android::ResStringPool_header::SORTED_FLAG;
    }
    std::shared_ptr<arsc::ResStringPoolBuilder> key_strings_builder =
        std::make_shared<arsc::ResStringPoolBuilder>(key_flags);
    rebuild_string_pool(*key_string_pool, key_old_to_new,
                        key_strings_builder.get());
    package_builder->set_key_strings(key_strings_builder);
    // Copy over all existing type data, which has been remapped by the step
    // above.
    auto search = collector.m_package_types.find(package);
    if (search != collector.m_package_types.end()) {
      auto types = search->second;
      for (auto& info : types) {
        std::string type_name;
        auto type_string_idx = info.spec->id - 1;
        if (apk::is_valid_string_idx(type_strings, type_string_idx)) {
          type_name = apk::get_string_from_pool(type_strings, type_string_idx);
        }
        if (!type_name.empty() &&
            config.canonical_entry_types.count(type_name) > 0) {
          TRACE(RES, 9, "Canonical entries enabled for ID 0x%x (%s)",
                info.spec->id, type_name.c_str());
          auto type_builder = std::make_shared<arsc::ResTableTypeProjector>(
              package->id, info.spec, info.configs, true);
          package_builder->add_type(type_builder);
        } else {
          package_builder->add_type(info);
        }
        if (dtohl(info.spec->entryCount) > 0) {
          kept_type_names.at(type_string_idx) = type_name;
          if (type_string_idx > last_kept_type_name) {
            last_kept_type_name = type_string_idx;
          }
        }
      }
    }

    // Copy all type names that were not fully deleted (or empty strings if they
    // were).
    std::shared_ptr<arsc::ResStringPoolBuilder> type_strings_builder =
        std::make_shared<arsc::ResStringPoolBuilder>(POOL_FLAGS(&type_strings));
    for (int i = 0; i <= last_kept_type_name; i++) {
      type_strings_builder->add_string(kept_type_names.at(i));
    }
    package_builder->set_type_strings(type_strings_builder);

    // Finally, preserve any chunks that we are not parsing.
    auto& unknown_chunks = collector.m_package_unknown_chunks.at(package);
    for (auto& header : unknown_chunks) {
      package_builder->add_chunk(header);
    }
    table_builder.add_package(package_builder);
  }
  android::Vector<char> serialized;
  table_builder.serialize(&serialized);

  // 6) Actually write the table to disk so changes take effect.
  TRACE(RES, 9, "Writing resources.arsc file, total size = %zu",
        serialized.size());
  m_arsc_len = write_serialized_data(serialized, std::move(m_f));
  mark_file_closed();
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
} // namespace

namespace {

// Copies the string data from the given pool to the builder and add additional
// strings. If needed, a remapper function can be run against the spans.
void rebuild_string_pool_with_addition(
    const android::ResStringPool& string_pool,
    const std::map<uint32_t, std::string>& id_to_new_strings,
    arsc::ResStringPoolBuilder* builder) {
  const auto original_string_count = string_pool.size();
  // Add all existing strings in original string pool to builder.
  for (size_t idx = 0; idx < original_string_count; idx++) {
    add_existing_string_to_builder(string_pool, builder, idx);
  }
  // Add additional strings to builder.
  for (size_t idx = 0; idx < id_to_new_strings.size(); idx++) {
    size_t additional_idx = original_string_count + idx;
    builder->add_string(id_to_new_strings.at(additional_idx));
  }
}

} // namespace

size_t ResourcesArscFile::obfuscate_resource_and_serialize(
    const std::vector<std::string>& /* unused */,
    const std::map<std::string, std::string>& filepath_old_to_new,
    const std::unordered_set<uint32_t>& allowed_types,
    const std::unordered_set<std::string>& keep_resource_prefixes,
    const std::unordered_set<std::string>& keep_resource_specific) {
  arsc::ResTableBuilder table_builder;

  // Find the global string pool and read its settings.
  GlobalStringPoolReader string_reader;
  string_reader.visit(m_f.data(), m_arsc_len);

  PackageStringRefCollector collector;
  collector.visit(m_f.data(), m_arsc_len);

  // 1) If filepath_old_to_new is not empty, we add new strings to global
  //    string pool and remap all string pool references. Otherwise we
  //    Just copy global string header as it is.
  if (filepath_old_to_new.empty()) {
    // Nothing to add/remap, just copy header as it is.
    table_builder.set_global_strings(string_reader.global_string_header());
  } else {
    auto string_pool = string_reader.global_strings();
    TRACE(RES, 9, "Global string pool has %zu styles and %zu total strings",
          string_pool->styleCount(), string_pool->size());
    auto flags = POOL_FLAGS_CLEAR_SORT(string_pool);
    // Build global old string to new string mapping and collect
    // new strings to add to global string pool
    std::map<std::string, uint32_t> global_new_strings_to_id;
    std::map<uint32_t, std::string> global_id_to_new_strings;
    std::map<uint32_t, uint32_t> global_old_to_new_id;
    uint32_t total_num_strings = string_pool->size();
    for (const auto& pair : filepath_old_to_new) {
      const auto& cur_new_string = pair.second;
      auto old_id = string_reader.get_string_idx(pair.first);
      always_assert_log(old_id >= string_pool->styleCount(),
                        "Don't support remapping of style.");
      if (!global_new_strings_to_id.count(cur_new_string)) {
        global_new_strings_to_id[cur_new_string] = total_num_strings;
        global_id_to_new_strings[total_num_strings] = cur_new_string;
        ++total_num_strings;
      }
      global_old_to_new_id[old_id] =
          global_new_strings_to_id.at(cur_new_string);
    }

    // Remap all Res_value structs
    auto remap_value = [&global_old_to_new_id](android::Res_value* value) {
      always_assert_log(value->dataType == android::Res_value::TYPE_STRING,
                        "Wrong data type for string remapping");
      auto old = dtohl(value->data);
      if (!global_old_to_new_id.count(old)) {
        return;
      }
      TRACE(RES, 9, "REMAP OLD %u", old);
      auto remapped_data = global_old_to_new_id.at(old);
      value->data = htodl(remapped_data);
    };
    for (const auto& value : collector.m_values) {
      remap_value(value);
    }

    // Actually build the new global ResStringPool.
    std::shared_ptr<arsc::ResStringPoolBuilder> global_strings_builder =
        std::make_shared<arsc::ResStringPoolBuilder>(flags);
    rebuild_string_pool_with_addition(*string_pool, global_id_to_new_strings,
                                      global_strings_builder.get());
    table_builder.set_global_strings(global_strings_builder);
  }

  // 2) Copy package settings as it is, anonymize resource name for application
  //    package (0x7fxxxxxx).
  auto start_package_id = PACKAGE_RESID_START >> PACKAGE_INDEX_BIT_SHIFT;
  size_t changed_resource_name = 0;
  for (auto& package_entries : collector.m_package_entries) {
    auto& package = package_entries.first;
    // Copy standard fields which will be unchanged in the output.
    std::shared_ptr<arsc::ResPackageBuilder> package_builder =
        std::make_shared<arsc::ResPackageBuilder>(package);

    if (start_package_id == package->id && !allowed_types.empty()) {
      // Set new string to be added to key string pool.
      auto key_string_pool = collector.m_package_key_strings.at(package);
      uint32_t new_key_string_index = key_string_pool->size();
      std::map<uint32_t, std::string> key_id_to_new_strings;
      key_id_to_new_strings[new_key_string_index] = RESOURCE_NAME_REMOVED;

      // Remap the entries.
      std::set<android::ResStringPool_ref*> refs;
      for (const auto& package_entry_pairs : package_entries.second) {
        // Only collect entries in allowed_types.
        if (!allowed_types.count(package_entry_pairs.first->id)) {
          continue;
        }
        const auto& package_type_entries = package_entry_pairs.second;
        refs.insert(package_type_entries.begin(), package_type_entries.end());
      }

      for (auto& ref : refs) {
        auto old = dtohl(ref->index);
        std::string old_string =
            apk::get_string_from_pool(*key_string_pool, old);
        if (keep_resource_specific.count(old_string) > 0 ||
            std::find_if(keep_resource_prefixes.begin(),
                         keep_resource_prefixes.end(),
                         [&](const std::string& v) {
                           return old_string.find(v) == 0;
                         }) != keep_resource_prefixes.end()) {
          // Resource name matches block criteria; don't change the name.
          continue;
        }
        TRACE(RES, 9, "REMAP OLD KEY %u", old);
        ref->index = htodl(new_key_string_index);
        ++changed_resource_name;
      }

      // Actually build the key strings pool.
      std::shared_ptr<arsc::ResStringPoolBuilder> key_strings_builder =
          std::make_shared<arsc::ResStringPoolBuilder>(
              POOL_FLAGS_CLEAR_SORT(key_string_pool));
      rebuild_string_pool_with_addition(*key_string_pool, key_id_to_new_strings,
                                        key_strings_builder.get());
      package_builder->set_key_strings(key_strings_builder);
    } else {
      // We are not in application package or there is no allowed type for
      // anonymizing, just use old key string pool header.
      package_builder->set_key_strings(
          collector.m_package_key_string_headers.at(package));
    }
    package_builder->set_type_strings(
        collector.m_package_type_string_headers.at(package));

    // Copy over all existing type data, which has been remapped by the step
    // above.
    auto search = collector.m_package_types.find(package);
    if (search != collector.m_package_types.end()) {
      auto types = search->second;
      for (auto& info : types) {
        package_builder->add_type(info);
      }
    }

    // Finally, preserve any chunks that we are not parsing.
    auto& unknown_chunks = collector.m_package_unknown_chunks.at(package);
    for (auto& header : unknown_chunks) {
      package_builder->add_chunk(header);
    }
    table_builder.add_package(package_builder);
  }
  android::Vector<char> serialized;
  table_builder.serialize(&serialized);

  // 3) Actually write the table to disk so changes take effect.
  TRACE(RES, 9, "Writing resources.arsc file, total size = %zu",
        serialized.size());
  m_arsc_len = write_serialized_data_with_expansion(serialized, std::move(m_f));
  mark_file_closed();
  return changed_resource_name;
}

std::vector<std::string> ResourcesArscFile::get_files_by_rid(
    uint32_t res_id, ResourcePathType /* unused */) {
  std::vector<std::string> ret;
  auto& table_snapshot = get_table_snapshot();
  std::vector<android::Res_value> out_values;
  table_snapshot.collect_resource_values(res_id, &out_values);
  for (size_t i = 0; i < out_values.size(); i++) {
    auto val = out_values[i];
    if (val.dataType == android::Res_value::TYPE_STRING) {
      // data is an index into string pool.
      auto s = table_snapshot.get_global_string(dtohl(val.data));
      if (is_resource_file(s)) {
        ret.emplace_back(s);
      }
    }
  }
  return ret;
}

uint64_t ResourcesArscFile::resource_value_count(uint32_t res_id) {
  std::vector<android::Res_value> out_values;
  auto& table_snapshot = get_table_snapshot();
  table_snapshot.collect_resource_values(res_id, &out_values);
  return out_values.size();
}

void ResourcesArscFile::walk_references_for_resource(
    uint32_t resID,
    ResourcePathType path_type,
    std::unordered_set<uint32_t>* nodes_visited,
    std::unordered_set<std::string>* potential_file_paths) {
  if (nodes_visited->find(resID) != nodes_visited->end()) {
    return;
  }
  nodes_visited->emplace(resID);

  auto& table_snapshot = get_table_snapshot();
  std::vector<android::Res_value> initial_values;
  table_snapshot.collect_resource_values(resID, &initial_values);

  std::stack<android::Res_value> nodes_to_explore;
  for (size_t index = 0; index < initial_values.size(); ++index) {
    nodes_to_explore.push(initial_values[index]);
  }

  while (!nodes_to_explore.empty()) {
    android::Res_value r = nodes_to_explore.top();
    nodes_to_explore.pop();
    if (r.dataType == android::Res_value::TYPE_STRING) {
      potential_file_paths->insert(
          table_snapshot.get_global_string(dtohl(r.data)));
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
    std::vector<android::Res_value> inner_values;
    table_snapshot.collect_resource_values(r.data, &inner_values);
    for (size_t index = 0; index < inner_values.size(); ++index) {
      nodes_to_explore.push(inner_values[index]);
    }
  }
}

void ResourcesArscFile::get_configurations(
    uint32_t package_id,
    const std::string& name,
    std::vector<android::ResTable_config>* configs) {
  auto& table_snapshot = get_table_snapshot();
  table_snapshot.get_configurations(package_id, name, configs);
}

std::set<android::ResTable_config> ResourcesArscFile::get_configs_with_values(
    uint32_t id) {
  auto& table_snapshot = get_table_snapshot();
  return table_snapshot.get_configs_with_values(id);
}

void ResourcesArscFile::delete_resource(uint32_t res_id) {
  m_ids_to_remove.emplace(res_id);
}

size_t ResourcesArscFile::package_count() {
  auto& table_snapshot = get_table_snapshot();
  return table_snapshot.package_count();
}

void ResourcesArscFile::collect_resid_values_and_hashes(
    const std::vector<uint32_t>& ids,
    std::map<size_t, std::vector<uint32_t>>* res_by_hash) {
  auto& table_snapshot = get_table_snapshot();
  for (uint32_t id : ids) {
    std::vector<android::Res_value> row_values;
    table_snapshot.collect_resource_values(id, &row_values);
    (*res_by_hash)[getHashFromValues(row_values)].push_back(id);
  }
}

bool ResourcesArscFile::resource_value_identical(uint32_t a_id, uint32_t b_id) {
  auto& table_snapshot = get_table_snapshot();
  return table_snapshot.are_values_identical(a_id, b_id);
}

ResourcesArscFile::ResourcesArscFile(const std::string& path)
    : m_f(RedexMappedFile::open(path, /* read_only= */ false)) {
  m_path = path;
  m_arsc_len = m_f.size();

  m_table_snapshot = std::make_unique<apk::TableSnapshot>(m_f, m_arsc_len);
  m_table_snapshot->gather_non_empty_resource_ids(&sorted_res_ids);

  // Build up maps to/from resource ID's and names
  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    uint32_t id = sorted_res_ids[index];
    auto name = m_table_snapshot->get_resource_name(id);
    id_to_name.emplace(id, name);
    name_to_ids[name].push_back(id);
  }
}

void ResourcesArscFile::mark_file_closed() {
  m_file_closed = true;
  m_table_snapshot.reset();
}

size_t ResourcesArscFile::get_length() const { return m_arsc_len; }

apk::TableSnapshot& ResourcesArscFile::get_table_snapshot() {
  always_assert_log(!m_file_closed && m_table_snapshot != nullptr,
                    "Backing file not opened");
  return *m_table_snapshot;
}

namespace {
// For a map keyed on ResTable_config instances, find the first key (if any)
// that is equivalent to the given config.
template <typename ValueType>
android::ResTable_config* find_equivalent_config_key(
    android::ResTable_config* to_find,
    const std::map<android::ResTable_config*, ValueType>& map) {
  for (const auto& pair : map) {
    if (arsc::are_configs_equivalent(to_find, pair.first)) {
      return pair.first;
    }
  }
  return nullptr;
}
} // namespace

size_t ResourcesArscFile::serialize() {
  // Serializing will apply pending deletions. This may greatly alter the
  // ResTable_typeSpec and ResTable_type structures emitted in the resulting
  // file. To do this, forward chunks from the parsed file to ResTableBuilder
  // and let that class make sense of what is to be omitted/retained in the
  // serialized output.
  apk::TableEntryParser table_parser = get_table_snapshot().get_parsed_table();
  // Re-assemble
  arsc::ResTableBuilder table_builder;
  table_builder.set_global_strings(table_parser.m_global_pool_header);
  for (auto& package : table_parser.m_packages) {
    auto package_id = dtohl(package->id);
    auto package_builder = std::make_shared<arsc::ResPackageBuilder>(package);
    package_builder->set_key_strings(
        table_parser.m_package_key_string_headers.at(package));
    // Append names of any new types
    auto type_strings_header =
        table_parser.m_package_type_string_headers.at(package);
    android::ResStringPool type_strings(
        type_strings_header, dtohl(type_strings_header->header.size));
    auto type_strings_builder = std::make_shared<arsc::ResStringPoolBuilder>(
        m_added_types.empty() ? POOL_FLAGS(&type_strings)
                              : POOL_FLAGS_CLEAR_SORT(&type_strings));
    rebuild_type_strings(package_id, type_strings, m_added_types,
                         type_strings_builder.get());
    package_builder->set_type_strings(type_strings_builder);
    // Copy existing types
    auto& type_infos = table_parser.m_package_types.at(package);
    for (auto& type_info : type_infos) {
      auto type_builder = std::make_shared<arsc::ResTableTypeProjector>(
          package_id, type_info.spec, type_info.configs);
      type_builder->remove_ids(m_ids_to_remove, m_nullify_removed);
      package_builder->add_type(type_builder);
    }
    // Append any new types
    for (auto& type_def : m_added_types) {
      // Refer to the re-parsed data at initial step to get full details of the
      // entries, flags and values for the new type.
      std::vector<uint32_t> flags;
      for (auto& id : type_def.source_res_ids) {
        flags.emplace_back(table_parser.m_res_id_to_flags.at(id));
      }
      auto type_definer = std::make_shared<arsc::ResTableTypeDefiner>(
          package_id, type_def.type_id, type_def.configs, flags);
      for (auto& id : type_def.source_res_ids) {
        auto& config_entries = table_parser.m_res_id_to_entries.at(id);
        for (auto& config : type_def.configs) {
          auto key = find_equivalent_config_key(config, config_entries);
          always_assert_log(key != nullptr,
                            "TypeDefinition %d is misconfigured; no equivalent "
                            "config found in table",
                            type_def.type_id);
          auto& data = config_entries.at(key);
          type_definer->add(config, data);
        }
      }
      package_builder->add_type(type_definer);
    }
    // Copy unknown chunks that we did not parse
    auto& unknown_chunks = table_parser.m_package_unknown_chunks.at(package);
    for (auto& header : unknown_chunks) {
      package_builder->add_chunk(header);
    }
    table_builder.add_package(package_builder);
  }
  android::Vector<char> serialized;
  table_builder.serialize(&serialized);

  m_arsc_len = write_serialized_data_with_expansion(serialized, std::move(m_f));
  mark_file_closed();
  return m_arsc_len;
}

namespace {
// Given an old -> new ID mapping, change all relevant values in entries/values:
// 1) Find all Res_value (whether in complex or non-complex) entries, remap the
//    data if it's a TYPE_REFERENCE or TYPE_ATTRIBUTE.
// 2) For complex entries, remap:
//    a) ResTable_map_entry's parent
//    b) For each ResTable_map, the name
class EntryRemapper : public arsc::ResourceTableVisitor {
 public:
  ~EntryRemapper() override {}

  explicit EntryRemapper(const std::map<uint32_t, uint32_t>& old_to_new)
      : m_old_to_new(old_to_new) {}

  void remap_value_impl(android::Res_value* value) {
    if (m_seen_values.count(value) > 0) {
      return;
    }
    m_seen_values.emplace(value);
    if (value->dataType == android::Res_value::TYPE_REFERENCE ||
        value->dataType == android::Res_value::TYPE_ATTRIBUTE) {
      auto search = m_old_to_new.find(dtohl(value->data));
      if (search != m_old_to_new.end()) {
        value->data = htodl(search->second);
      }
    }
  }

  void remap_ref_impl(android::ResTable_ref* ref) {
    if (m_seen_refs.count(ref) > 0) {
      return;
    }
    m_seen_refs.emplace(ref);
    auto search = m_old_to_new.find(dtohl(ref->ident));
    if (search != m_old_to_new.end()) {
      ref->ident = htodl(search->second);
    }
  }

  bool visit_entry(android::ResTable_package* /* unused */,
                   android::ResTable_typeSpec* /* unused */,
                   android::ResTable_type* /* unused */,
                   android::ResTable_entry* /* unused */,
                   android::Res_value* value) override {
    remap_value_impl(value);
    return true;
  }

  bool visit_map_entry(android::ResTable_package* /* unused */,
                       android::ResTable_typeSpec* /* unused */,
                       android::ResTable_type* /* unused */,
                       android::ResTable_map_entry* entry) override {
    remap_ref_impl(&entry->parent);
    return true;
  }

  bool visit_map_value(android::ResTable_package* /* unused */,
                       android::ResTable_typeSpec* /* unused */,
                       android::ResTable_type* /* unused */,
                       android::ResTable_map_entry* /* unused */,
                       android::ResTable_map* value) override {
    remap_value_impl(&value->value);
    remap_ref_impl(&value->name);
    return true;
  }

 private:
  const std::map<uint32_t, uint32_t>& m_old_to_new;
  // Tolerate a "canonicalized" version of a type, to make sure we don't double
  // remap.
  std::unordered_set<android::Res_value*> m_seen_values;
  std::unordered_set<android::ResTable_ref*> m_seen_refs;
};
} // namespace

void ResourcesArscFile::remap_ids(
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) {
  EntryRemapper remapper(old_to_remapped_ids);
  // Note: file is opened for writing. Visitor will in place change the data
  // (without altering any data sizes).
  remapper.visit(m_f.data(), m_arsc_len);
}

void ResourcesArscFile::get_type_names(std::vector<std::string>* type_names) {
  auto& table_snapshot = get_table_snapshot();
  table_snapshot.get_type_names(APPLICATION_PACKAGE, type_names);
}

std::unordered_set<uint32_t> ResourcesArscFile::get_types_by_name(
    const std::unordered_set<std::string>& type_names) {
  auto& table_snapshot = get_table_snapshot();
  std::vector<std::string> all_types;
  table_snapshot.get_type_names(APPLICATION_PACKAGE, &all_types);

  std::unordered_set<uint32_t> type_ids;
  for (size_t i = 0; i < all_types.size(); ++i) {
    if (type_names.count(all_types.at(i)) == 1) {
      type_ids.emplace((i + 1) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

std::unordered_set<uint32_t> ResourcesArscFile::get_types_by_name_prefixes(
    const std::unordered_set<std::string>& type_name_prefixes) {
  auto& table_snapshot = get_table_snapshot();
  std::vector<std::string> all_types;
  table_snapshot.get_type_names(APPLICATION_PACKAGE, &all_types);

  std::unordered_set<uint32_t> type_ids;
  for (size_t i = 0; i < all_types.size(); ++i) {
    const auto& type_name = all_types.at(i);
    if (std::find_if(type_name_prefixes.begin(), type_name_prefixes.end(),
                     [&](const std::string& prefix) {
                       return type_name.find(prefix) != std::string::npos;
                     }) != type_name_prefixes.end()) {
      type_ids.emplace((i + 1) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

namespace {

// For the given ID, look up values in all configs for string data. Any
// references encountered will be resolved and handled recursively.
void resolve_string_index_for_id(apk::TableSnapshot& table_snapshot,
                                 uint32_t id,
                                 std::unordered_set<uint32_t>* seen,
                                 std::set<uint32_t>* out_idx) {
  // Annoyingly, Android build tools allow references to have cycles in them
  // without failing at build time. At runtime, such a situation would just loop
  // a fixed number of times (https://fburl.com/xmckadjk) but we'll keep track
  // of a seen list.
  if (seen->count(id) > 0) {
    return;
  }
  seen->insert(id);
  std::vector<android::Res_value> values;
  table_snapshot.collect_resource_values(id, &values);
  for (size_t i = 0; i < values.size(); i++) {
    auto value = values[i];
    if (value.dataType == android::Res_value::TYPE_STRING) {
      out_idx->insert(value.data);
    } else if (value.dataType == android::Res_value::TYPE_REFERENCE) {
      resolve_string_index_for_id(table_snapshot, value.data, seen, out_idx);
    }
  }
}

} // namespace

std::vector<std::string> ResourcesArscFile::get_resource_strings_by_name(
    const std::string& res_name) {
  std::vector<std::string> ret;
  auto& table_snapshot = get_table_snapshot();
  auto it = name_to_ids.find(res_name);
  if (it != name_to_ids.end()) {
    std::unordered_set<uint32_t> seen;
    std::set<uint32_t> string_idx;
    for (uint32_t id : it->second) {
      resolve_string_index_for_id(table_snapshot, id, &seen, &string_idx);
    }
    ret.reserve(string_idx.size());
    for (const auto& i : string_idx) {
      if (table_snapshot.is_valid_global_string_idx(i)) {
        ret.push_back(table_snapshot.get_global_string(i));
      }
    }
  }
  return ret;
}

ResourcesArscFile::~ResourcesArscFile() {}
