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

bool is_binary_xml(const void* data, size_t size) {
  if (size < sizeof(android::ResChunk_header)) {
    return false;
  }
  return dtohs(((android::ResChunk_header*)data)->type) ==
         android::RES_XML_TYPE;
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

bool is_raw_resource(const std::string& filename) {
  return filename.find("/res/raw/") != std::string::npos ||
         filename.find("/res/raw-") != std::string::npos;
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

