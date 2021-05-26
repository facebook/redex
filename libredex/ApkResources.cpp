/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApkResources.h"

#include "RedexMappedFile.h"
#include "RedexResources.h"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>
#include <map>

#include "Macros.h"
#if IS_WINDOWS
#include "CompatWindows.h"
#include <io.h>
#include <share.h>
#endif

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/TypeHelpers.h"

#include "Debug.h"
#include "DexUtil.h"
#include "IOUtil.h"
#include "Macros.h"
#include "ReadMaybeMapped.h"
#include "Trace.h"

// Workaround for inclusion order, when compiling on Windows (#defines NO_ERROR
// as 0).
#ifdef NO_ERROR
#undef NO_ERROR
#endif

namespace {
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
          size_t start = 0;
          size_t end = 0;
          while ((end = text.find(';', start)) != std::string::npos) {
            tag_info.authority_classes.insert(java_names::external_to_internal(
                text.substr(start, end - start)));
            start = end + 1;
          }
          tag_info.authority_classes.insert(
              java_names::external_to_internal(text.substr(start)));
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
} // namespace

boost::optional<int32_t> ApkResources::get_min_sdk() {
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
  push_short(*out_data, android::RES_XML_TYPE);
  push_short(*out_data, chunk_size);
  auto total_size =
      chunk_size + serialized_nodes.size() + serialized_pool.size();
  push_long(*out_data, total_size);

  out_data->appendVector(serialized_pool);
  out_data->appendVector(serialized_nodes);

  *out_num_renamed = num_replaced;
  return android::OK;
}

int ApkResources::rename_classes_in_layout(
    const std::string& file_path,
    const std::map<std::string, std::string>& rename_map,
    size_t* out_num_renamed,
    ssize_t* out_size_delta) {
  RedexMappedFile f = RedexMappedFile::open(file_path, /* read_only= */ false);
  size_t len = f.size();

  android::Vector<char> serialized;
  auto status = replace_in_xml_string_pool(f.data(), f.size(), rename_map,
                                           &serialized, out_num_renamed);

  if (*out_num_renamed == 0 || status != android::OK) {
    return status;
  }

  write_serialized_data(serialized, std::move(f));
  *out_size_delta = serialized.size() - len;
  return android::OK;
}

void ApkResources::rename_classes_in_layouts(
    const std::map<std::string, std::string>& rename_map) {
  ssize_t layout_bytes_delta = 0;
  size_t num_layout_renamed = 0;
  auto xml_files = get_xml_files(m_directory + "/res");
  for (const auto& path : xml_files) {
    if (is_raw_resource(path)) {
      continue;
    }
    size_t num_renamed = 0;
    ssize_t out_delta = 0;
    TRACE(RES, 3, "Begin rename Views in layout %s", path.c_str());
    rename_classes_in_layout(path, rename_map, &num_renamed, &out_delta);
    TRACE(RES, 3, "Renamed %zu ResStringPool entries in layout %s", num_renamed,
          path.c_str());
    layout_bytes_delta += out_delta;
    num_layout_renamed += num_renamed;
  }
  TRACE(RES, 2, "Renamed %zu ResStringPool entries, delta %zi bytes",
        num_layout_renamed, layout_bytes_delta);
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
