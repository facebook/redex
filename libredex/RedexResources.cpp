/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexResources.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
#include "Macros.h"
#include "ReadMaybeMapped.h"
#include "StringUtil.h"
#include "Trace.h"
#include "WorkQueue.h"

// Workaround for inclusion order, when compiling on Windows (#defines NO_ERROR
// as 0).
#ifdef NO_ERROR
#undef NO_ERROR
#endif

namespace {

constexpr size_t MIN_CLASSNAME_LENGTH = 10;
constexpr size_t MAX_CLASSNAME_LENGTH = 500;

const uint32_t PACKAGE_RESID_START = 0x7f000000;

constexpr decltype(redex_parallel::default_num_threads()) kReadXMLThreads = 4u;
constexpr decltype(redex_parallel::default_num_threads()) kReadNativeThreads =
    2u;

using path_t = boost::filesystem::path;
using dir_iterator = boost::filesystem::directory_iterator;
using rdir_iterator = boost::filesystem::recursive_directory_iterator;

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

namespace {

std::string dotname_to_dexname(const std::string& classname) {
  std::string dexname;
  dexname.reserve(classname.size() + 2);
  dexname += 'L';
  dexname += classname;
  dexname += ';';
  std::replace(dexname.begin(), dexname.end(), '.', '/');
  return dexname;
}

bool is_binary_xml(const void* data, size_t size) {
  if (size < sizeof(android::ResChunk_header)) {
    return false;
  }
  return dtohs(((android::ResChunk_header*)data)->type) ==
         android::RES_XML_TYPE;
}

std::unordered_set<uint32_t> extract_xml_reference_attributes(
    const std::string& file_contents, const std::string& filename) {
  android::ResXMLTree parser;
  parser.setTo(file_contents.data(), file_contents.size());
  std::unordered_set<uint32_t> result;
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

} // namespace

/**
 * Follows the reference links for a resource for all configurations.
 * Outputs all the nodes visited, as well as all the string values seen.
 */
void walk_references_for_resource(
    const android::ResTable& table,
    uint32_t resID,
    std::unordered_set<uint32_t>* nodes_visited,
    std::unordered_set<std::string>* leaf_string_values) {
  if (nodes_visited->find(resID) != nodes_visited->end()) {
    return;
  }
  nodes_visited->emplace(resID);

  ssize_t pkg_index = table.getResourcePackageIndex(resID);

  android::Vector<android::Res_value> initial_values;
  table.getAllValuesForResource(resID, initial_values);

  std::stack<android::Res_value> nodes_to_explore;
  for (size_t index = 0; index < initial_values.size(); ++index) {
    nodes_to_explore.push(initial_values[index]);
  }

  while (!nodes_to_explore.empty()) {
    android::Res_value r = nodes_to_explore.top();
    nodes_to_explore.pop();

    if (r.dataType == android::Res_value::TYPE_STRING) {
      android::String8 str = table.getString8FromIndex(pkg_index, r.data);
      leaf_string_values->insert(std::string(str.string()));
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
    table.getAllValuesForResource(r.data, inner_values);
    for (size_t index = 0; index < inner_values.size(); ++index) {
      nodes_to_explore.push(inner_values[index]);
    }
  }
}

namespace {
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
              dotname_to_dexname(classname));
        }
        std::string app_factory_cls =
            get_string_attribute_value(parser, app_component_factory);
        if (!app_factory_cls.empty()) {
          manifest_classes.application_classes.emplace(
              dotname_to_dexname(app_factory_cls));
        }
      } else if (tag == instrumentation) {
        std::string classname = get_string_attribute_value(parser, name);
        always_assert(classname.size());
        manifest_classes.instrumentation_classes.emplace(
            dotname_to_dexname(classname));
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
                                  dotname_to_dexname(classname),
                                  export_attribute,
                                  permission_attribute,
                                  protection_level_attribute);

        if (tag == provider) {
          std::string text = get_string_attribute_value(parser, authorities);
          size_t start = 0;
          size_t end = 0;
          while ((end = text.find(';', start)) != std::string::npos) {
            tag_info.authority_classes.insert(
                dotname_to_dexname(text.substr(start, end - start)));
            start = end + 1;
          }
          tag_info.authority_classes.insert(
              dotname_to_dexname(text.substr(start)));
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
    std::unordered_set<std::string>& out_classes,
    std::unordered_multimap<std::string, std::string>& out_attributes) {
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
        out_classes.insert(converted);
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
              out_attributes.emplace(fully_qualified,
                                     convert_from_string16(s16));
            }
          }
        }
      }
    } else if (type == android::ResXMLParser::START_NAMESPACE) {
      auto id = parser.getNamespaceUriID();
      size_t len;
      auto prefix = parser.getNamespacePrefix(&len);
      ;
      namespace_prefix_map.emplace(
          id, convert_from_string16(android::String16(prefix, len)));
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
}

/*
 * Returns all strings that look like java class names from a native library.
 *
 * Return values will be formatted the way that the dex spec formats class
 * names:
 *
 *   "Ljava/lang/String;"
 *
 */
std::unordered_set<std::string> extract_classes_from_native_lib(
    const char* data, size_t size) {
  std::unordered_set<std::string> classes;
  char buffer[MAX_CLASSNAME_LENGTH + 2]; // +2 for the trailing ";\0"
  const char* inptr = data;
  const char* end = inptr + size;

  while (inptr < end) {
    char* outptr = buffer;
    size_t length = 0;
    // All classnames start with a package, which starts with a lowercase
    // letter. Some of them are preceded by an 'L' and followed by a ';' in
    // native libraries while others are not.
    if ((*inptr >= 'a' && *inptr <= 'z') || *inptr == 'L') {

      if (*inptr != 'L') {
        *outptr++ = 'L';
        length++;
      }

      while (inptr < end &&
             ((*inptr >= 'a' && *inptr <= 'z') ||
              (*inptr >= 'A' && *inptr <= 'Z') ||
              (*inptr >= '0' && *inptr <= '9') || *inptr == '/' ||
              *inptr == '_' || *inptr == '$') &&
             length < MAX_CLASSNAME_LENGTH) {
        *outptr++ = *inptr++;
        length++;
      }
      if (length >= MIN_CLASSNAME_LENGTH) {
        *outptr++ = ';';
        *outptr = '\0';
        classes.insert(std::string(buffer));
      }
    }
    inptr++;
  }
  return classes;
}

} // namespace

// For external testing.
std::unordered_set<std::string> extract_classes_from_native_lib(
    const std::string& lib_contents) {
  return extract_classes_from_native_lib(lib_contents.data(),
                                         lib_contents.size());
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

void write_entire_file(const std::string& filename,
                       const std::string& contents) {
  std::ofstream out(filename, std::ofstream::binary);
  out << contents;
}

boost::optional<int32_t> get_min_sdk(const std::string& manifest_filename) {
  const std::string& manifest = read_entire_file(manifest_filename);

  if (manifest.empty()) {
    fprintf(stderr, "WARNING: Cannot find/read the manifest file %s\n",
            manifest_filename.c_str());
    return boost::none;
  }

  android::ResXMLTree parser;
  parser.setTo(manifest.data(), manifest.size());

  if (parser.getError() != android::NO_ERROR) {
    fprintf(stderr, "WARNING: Failed to parse the manifest file %s\n",
            manifest_filename.c_str());
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

ManifestClassInfo get_manifest_class_info(const std::string& apk_dir) {
  std::string manifest =
      (boost::filesystem::path(apk_dir) / "AndroidManifest.xml").string();
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

std::unordered_set<std::string> get_files_by_suffix(
    const std::string& directory, const std::string& suffix) {
  std::unordered_set<std::string> files;
  path_t dir(directory);

  if (exists(dir) && is_directory(dir)) {
    for (auto it = dir_iterator(dir); it != dir_iterator(); ++it) {
      auto const& entry = *it;
      const path_t& entry_path = entry.path();

      if (is_regular_file(entry_path) &&
          ends_with(entry_path.string().c_str(), suffix.c_str())) {
        files.emplace(entry_path.string());
      }

      if (is_directory(entry_path)) {
        std::unordered_set<std::string> sub_files =
            get_files_by_suffix(entry_path.string(), suffix);

        files.insert(sub_files.begin(), sub_files.end());
      }
    }
  }
  return files;
}

std::unordered_set<std::string> get_xml_files(const std::string& directory) {
  return get_files_by_suffix(directory, ".xml");
}

std::unordered_set<uint32_t> get_resources_by_name_prefix(
    const std::vector<std::string>& prefixes,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids) {
  std::unordered_set<uint32_t> found_resources;

  for (const auto& pair : name_to_ids) {
    for (const auto& prefix : prefixes) {
      if (boost::algorithm::starts_with(pair.first, prefix)) {
        found_resources.insert(pair.second.begin(), pair.second.end());
      }
    }
  }

  return found_resources;
}

namespace {

void ensure_file_contents(const std::string& file_contents,
                          const std::string& filename) {
  if (file_contents.empty()) {
    fprintf(stderr, "Unable to read file: %s\n", filename.data());
    throw std::runtime_error("Unable to read file: " + filename);
  }
}

} // namespace

bool is_raw_resource(const std::string& filename) {
  return filename.find("/res/raw/") != std::string::npos ||
         filename.find("/res/raw-") != std::string::npos;
}

std::unordered_set<uint32_t> get_xml_reference_attributes(
    const std::string& filename) {
  if (is_raw_resource(filename)) {
    std::unordered_set<uint32_t> empty;
    return empty;
  }
  std::string file_contents = read_entire_file(filename);
  ensure_file_contents(file_contents, filename);
  return extract_xml_reference_attributes(file_contents, filename);
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

} // namespace

int inline_xml_reference_attributes(
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
    write_entire_file(filename, file_contents);
  }

  return num_values_inlined;
}

void remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) {
  if (is_raw_resource(filename)) {
    return;
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
    write_entire_file(filename, file_contents);
  }
}

namespace {

template <typename Fn>
void find_resource_xml_files(const std::string& apk_directory,
                             const std::vector<std::string>& skip_dirs_prefixes,
                             Fn handler) {
  std::string root = apk_directory + std::string("/res");
  path_t res(root);

  if (exists(res) && is_directory(res)) {
    for (auto it = dir_iterator(res); it != dir_iterator(); ++it) {
      auto const& entry = *it;
      const path_t& entry_path = entry.path();

      if (is_directory(entry_path)) {
        auto matches_prefix = [&skip_dirs_prefixes](const auto& p) {
          const auto& str = p.string();
          for (const auto& prefix : skip_dirs_prefixes) {
            if (str.find(prefix) == 0) {
              return true;
            }
          }
          return false;
        };
        if (matches_prefix(entry_path.filename())) {
          continue;
        }

        for (auto lit = dir_iterator(entry_path); lit != dir_iterator();
             ++lit) {
          const path_t& resource_path = lit->path();
          if (is_regular_file(resource_path) &&
              ends_with(resource_path.string().c_str(), ".xml")) {
            handler(resource_path.string());
          }
        }
      }
    }
  }
}

} // namespace

void collect_layout_classes_and_attributes_for_file(
    const std::string& file_path,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>& out_classes,
    std::unordered_multimap<std::string, std::string>& out_attributes) {
  redex::read_file_with_contents(file_path, [&](const char* data, size_t size) {
    extract_classes_from_layout(data, size, attributes_to_read, out_classes,
                                out_attributes);
  });
}

void collect_layout_classes_and_attributes(
    const std::string& apk_directory,
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>& out_classes,
    std::unordered_multimap<std::string, std::string>& out_attributes) {
  auto collect_fn = [&](const std::vector<std::string>& prefixes) {
    std::mutex out_mutex;
    workqueue_run<std::string>(
        [&](sparta::SpartaWorkerState<std::string>* worker_state,
            const std::string& input) {
          if (input.empty()) {
            // Dispatcher, find files and create tasks.
            find_resource_xml_files(apk_directory, prefixes,
                                    [&](const std::string& file) {
                                      worker_state->push_task(file);
                                    });
            return;
          }

          std::unordered_set<std::string> local_out_classes;
          std::unordered_multimap<std::string, std::string>
              local_out_attributes;
          collect_layout_classes_and_attributes_for_file(
              input, attributes_to_read, local_out_classes,
              local_out_attributes);
          if (!local_out_classes.empty() || !local_out_attributes.empty()) {
            std::unique_lock<std::mutex> lock(out_mutex);
            // C++17: use merge to avoid copies.
            out_classes.insert(local_out_classes.begin(),
                               local_out_classes.end());
            out_attributes.insert(local_out_attributes.begin(),
                                  local_out_attributes.end());
          }
        },
        std::vector<std::string>{""},
        std::min(redex_parallel::default_num_threads(), kReadXMLThreads),
        /*push_tasks_while_running=*/true);
  };

  collect_fn({
      // Animations do not have references (that we track).
      "anim",
      // Colors do not have references.
      "color",
      // There are usually a lot of drawable resources, non of
      // which contain any code references.
      "drawable",
      // Raw would not contain binary XML.
      "raw",
  });

  if (slow_invariants_debug) {
    TRACE(RES, 1,
          "Checking collect_layout_classes_and_attributes filter assumption");
    size_t out_classes_size = out_classes.size();
    size_t out_attributes_size = out_attributes.size();

    // Comparison is complicated, as out_attributes is a multi-map.
    // Assume that the inputs were empty, for simplicity.
    out_classes.clear();
    out_attributes.clear();

    collect_fn({});
    size_t new_out_classes_size = out_classes.size();
    size_t new_out_attributes_size = out_attributes.size();
    redex_assert(out_classes_size == new_out_classes_size);
    redex_assert(out_attributes_size == new_out_attributes_size);
  }
}

std::unordered_set<std::string> get_layout_classes(
    const std::string& apk_directory) {
  std::unordered_set<std::string> out_classes;
  // No attributes to read, empty set
  std::unordered_set<std::string> attributes_to_read;
  std::unordered_multimap<std::string, std::string> unused;
  collect_layout_classes_and_attributes(apk_directory, attributes_to_read,
                                        out_classes, unused);
  return out_classes;
}

std::set<std::string> multimap_values_to_set(
    const std::unordered_multimap<std::string, std::string>& map,
    const std::string& key) {
  std::set<std::string> result;
  auto range = map.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    result.emplace(it->second);
  }
  return result;
}

namespace {

/**
 * Return a list of all the .so files in /lib
 */
template <typename Fn>
void find_native_library_files(const std::string& apk_directory, Fn handler) {
  std::string lib_root = apk_directory + std::string("/lib");
  std::string library_extension(".so");

  path_t lib(lib_root);

  if (exists(lib) && is_directory(lib)) {
    for (auto it = rdir_iterator(lib); it != rdir_iterator(); ++it) {
      auto const& entry = *it;
      const path_t& entry_path = entry.path();
      if (is_regular_file(entry_path) &&
          ends_with(entry_path.filename().string().c_str(),
                    library_extension.c_str())) {
        handler(entry_path.string());
      }
    }
  }
}

} // namespace

/**
 * Return all potential java class names located in native libraries.
 */
std::unordered_set<std::string> get_native_classes(
    const std::string& apk_directory) {
  std::mutex out_mutex;
  std::unordered_set<std::string> all_classes;
  workqueue_run<std::string>(
      [&](sparta::SpartaWorkerState<std::string>* worker_state,
          const std::string& input) {
        if (input.empty()) {
          // Dispatcher, find files and create tasks.
          find_native_library_files(
              apk_directory,
              [&](const std::string& file) { worker_state->push_task(file); });
          return;
        }

        redex::read_file_with_contents(
            input,
            [&](const char* data, size_t size) {
              std::unordered_set<std::string> classes_from_native =
                  extract_classes_from_native_lib(data, size);
              if (!classes_from_native.empty()) {
                std::unique_lock<std::mutex> lock(out_mutex);
                // C++17: use merge to avoid copies.
                all_classes.insert(classes_from_native.begin(),
                                   classes_from_native.end());
              }
            },
            64 * 1024);
      },
      std::vector<std::string>{""},
      std::min(redex_parallel::default_num_threads(), kReadNativeThreads),
      /*push_tasks_while_running=*/true);
  return all_classes;
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

int replace_in_xml_string_pool(
    const void* data,
    const size_t len,
    const std::map<std::string, std::string>& shortened_names,
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

    auto replacement = shortened_names.find(existing_str);
    if (replacement == shortened_names.end()) {
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

int rename_classes_in_layout(
    const std::string& file_path,
    const std::map<std::string, std::string>& shortened_names,
    size_t* out_num_renamed,
    ssize_t* out_size_delta) {
  RedexMappedFile f = RedexMappedFile::open(file_path, /* read_only= */ false);
  size_t len = f.size();

  android::Vector<char> serialized;
  auto status = replace_in_xml_string_pool(f.data(), f.size(), shortened_names,
                                           &serialized, out_num_renamed);

  if (*out_num_renamed == 0 || status != android::OK) {
    return status;
  }

  write_serialized_data(serialized, std::move(f));
  *out_size_delta = serialized.size() - len;
  return android::OK;
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
