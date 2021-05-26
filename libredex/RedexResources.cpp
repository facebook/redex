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

#include "BundleResources.h"
#include "Macros.h"

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/TypeHelpers.h"

#include "ApkResources.h"
#include "Debug.h"
#include "IOUtil.h"
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
} // namespace

std::unique_ptr<AndroidResources> create_resource_reader(
    const std::string& directory) {
// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#ifdef HAS_PROTOBUF
  std::string bundle_config =
      (boost::filesystem::path(directory) / "BundleConfig.pb").string();
  if (boost::filesystem::exists(bundle_config)) {
    return std::make_unique<BundleResources>(directory);
  } else {
    return std::make_unique<ApkResources>(directory);
  }
#else
  return std::make_unique<ApkResources>(directory);
#endif // HAS_PROTOBUF
}

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
    write_string_to_file(filename, file_contents);
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
    write_string_to_file(filename, file_contents);
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

