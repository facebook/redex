/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexResources.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ApkResources.h"
#include "BundleResources.h"
#include "Debug.h"
#include "DetectBundle.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
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
} // namespace

std::unique_ptr<AndroidResources> create_resource_reader(
    const std::string& directory) {
// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#ifdef HAS_PROTOBUF
  if (has_bundle_config(directory)) {
    return std::make_unique<BundleResources>(directory);
  } else {
    return std::make_unique<ApkResources>(directory);
  }
#else
  return std::make_unique<ApkResources>(directory);
#endif // HAS_PROTOBUF
}

namespace {
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

void parse_authorities(const std::string& text,
                       std::unordered_set<std::string>* authority_classes) {
  size_t start = 0;
  size_t end = 0;
  while ((end = text.find(';', start)) != std::string::npos) {
    authority_classes->insert(
        java_names::external_to_internal(text.substr(start, end - start)));
    start = end + 1;
  }
  authority_classes->insert(
      java_names::external_to_internal(text.substr(start)));
}

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
          boost::ends_with(entry_path.string(), suffix)) {
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
void find_resource_xml_files(const std::string& dir,
                             const std::vector<std::string>& skip_dirs_prefixes,
                             Fn handler) {
  path_t res(dir);
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
              boost::ends_with(resource_path.string(), ".xml")) {
            handler(resource_path.string());
          }
        }
      } else {
        // In case input APK has resource file path changed and not in usual
        // format.
        // TODO(T126661220): this disabled performance improvement to read less
        // resource files, it would be better if we have mapping file to map
        // back resource file names.
        if (is_regular_file(entry_path) &&
            boost::ends_with(entry_path.string(), ".xml")) {
          handler(entry_path.string());
        }
      }
    }
  }
}
} // namespace

void AndroidResources::collect_layout_classes_and_attributes(
    const std::unordered_set<std::string>& attributes_to_read,
    std::unordered_set<std::string>* out_classes,
    std::unordered_multimap<std::string, std::string>* out_attributes) {
  auto collect_fn = [&](const std::vector<std::string>& prefixes) {
    std::mutex out_mutex;
    workqueue_run<std::string>(
        [&](sparta::SpartaWorkerState<std::string>* worker_state,
            const std::string& input) {
          if (input.empty()) {
            // Dispatcher, find files and create tasks.
            auto directories = find_res_directories();
            for (const auto& dir : directories) {
              TRACE(RES, 9,
                    "Scanning %s for xml files for classes and attributes",
                    dir.c_str());
              find_resource_xml_files(dir, prefixes,
                                      [&](const std::string& file) {
                                        worker_state->push_task(file);
                                      });
            }

            return;
          }

          std::unordered_set<std::string> local_out_classes;
          std::unordered_multimap<std::string, std::string>
              local_out_attributes;
          collect_layout_classes_and_attributes_for_file(
              input, attributes_to_read, &local_out_classes,
              &local_out_attributes);
          if (!local_out_classes.empty() || !local_out_attributes.empty()) {
            std::unique_lock<std::mutex> lock(out_mutex);
            // C++17: use merge to avoid copies.
            out_classes->insert(local_out_classes.begin(),
                                local_out_classes.end());
            out_attributes->insert(local_out_attributes.begin(),
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
    size_t out_classes_size = out_classes->size();
    size_t out_attributes_size = out_attributes->size();

    // Comparison is complicated, as out_attributes is a multi-map.
    // Assume that the inputs were empty, for simplicity.
    out_classes->clear();
    out_attributes->clear();

    collect_fn({});
    size_t new_out_classes_size = out_classes->size();
    size_t new_out_attributes_size = out_attributes->size();
    redex_assert(out_classes_size == new_out_classes_size);
    redex_assert(out_attributes_size == new_out_attributes_size);
  }
}

void AndroidResources::collect_xml_attribute_string_values(
    std::unordered_set<std::string>* out) {
  std::mutex out_mutex;
  workqueue_run<std::string>(
      [&](sparta::SpartaWorkerState<std::string>* worker_state,
          const std::string& input) {
        if (input.empty()) {
          // Dispatcher, find files and create tasks.
          auto directories = find_res_directories();
          for (const auto& dir : directories) {
            TRACE(RES, 9, "Scanning %s for xml files for attribute values",
                  dir.c_str());
            find_resource_xml_files(dir, {}, [&](const std::string& file) {
              worker_state->push_task(file);
            });
          }

          return;
        }

        std::unordered_set<std::string> local_out_values;
        collect_xml_attribute_string_values_for_file(input, &local_out_values);
        if (!local_out_values.empty()) {
          std::unique_lock<std::mutex> lock(out_mutex);
          // C++17: use merge to avoid copies.
          out->insert(local_out_values.begin(), local_out_values.end());
        }
      },
      std::vector<std::string>{""},
      std::min(redex_parallel::default_num_threads(), kReadXMLThreads),
      /*push_tasks_while_running=*/true);
}

void AndroidResources::rename_classes_in_layouts(
    const std::map<std::string, std::string>& rename_map) {
  workqueue_run<std::string>(
      [&](sparta::SpartaWorkerState<std::string>* worker_state,
          const std::string& input) {
        if (input.empty()) {
          // Dispatcher, find files and create tasks.
          auto directories = find_res_directories();
          for (const auto& dir : directories) {
            auto xml_files = get_xml_files(dir);
            for (const auto& path : xml_files) {
              if (!is_raw_resource(path)) {
                worker_state->push_task(path);
              }
            }
          }
          return;
        }
        size_t num_renamed = 0;
        TRACE(RES, 3, "Begin rename Views in layout %s", input.c_str());
        bool result = rename_classes_in_layout(input, rename_map, &num_renamed);
        TRACE(RES, 3, "%sRenamed %zu class names in file %s",
              (result ? "" : "FAILED: "), num_renamed, input.c_str());
      },
      std::vector<std::string>{""},
      std::min(redex_parallel::default_num_threads(), kReadXMLThreads),
      /*push_tasks_while_running=*/true);
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
void find_native_library_files(const std::string& lib_root, Fn handler) {
  std::string library_extension(".so");

  path_t lib(lib_root);

  if (exists(lib) && is_directory(lib)) {
    for (auto it = rdir_iterator(lib); it != rdir_iterator(); ++it) {
      auto const& entry = *it;
      const path_t& entry_path = entry.path();
      if (is_regular_file(entry_path) &&
          boost::ends_with(entry_path.filename().string(), library_extension)) {
        TRACE(RES, 9, "Checking lib: %s", entry_path.string().c_str());
        handler(entry_path.string());
      }
    }
  }
}

} // namespace

/**
 * Return all potential java class names located in native libraries.
 */
std::unordered_set<std::string> AndroidResources::get_native_classes() {
  std::mutex out_mutex;
  std::unordered_set<std::string> all_classes;
  workqueue_run<std::string>(
      [&](sparta::SpartaWorkerState<std::string>* worker_state,
          const std::string& input) {
        if (input.empty()) {
          // Dispatcher, find files and create tasks.
          auto directories = find_lib_directories();
          for (const auto& dir : directories) {
            TRACE(RES, 9, "Scanning %s for so files for class names",
                  dir.c_str());
            find_native_library_files(dir, [&](const std::string& file) {
              worker_state->push_task(file);
            });
          }
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

bool AndroidResources::can_obfuscate_xml_file(
    const std::unordered_set<std::string>& allowed_types,
    const std::string& dirname) {
  for (const auto& type : allowed_types) {
    auto path = RES_DIRECTORY + std::string("/") + type;
    if (dirname.find(path) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void ResourceTableFile::remove_unreferenced_strings(
    const ResourceConfig& config) {
  // Intentionally left empty, proto resource table will not contain a relevant
  // structure to prune.
}
