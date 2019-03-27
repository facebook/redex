/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Shorten.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Walkers.h"
#include "Warning.h"

constexpr const char* METRIC_SHORTENED_STRINGS = "num_shortened_strings";
constexpr const char* METRIC_BYTES_SAVED = "num_shortening_bytes_saved";

static bool maybe_file_name(const char* str, size_t len) {
  if (len < 5) return false;
  return strncmp(str + len - 5, ".java", 5) == 0;
}

static bool is_reasonable_string(const char* str, size_t len) {
  std::vector<char> avoid = {'\n', '\t', ':', ','};
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) {
    for (auto c : avoid) {
      if (str[i] == c) {
        return false;
      }
    }
  }
  return true;
}

DexString* get_suitable_string(std::unordered_set<DexString*>& set,
                               std::vector<DexString*>& dex_strings) {
  while (dex_strings.size()) {
    DexString* val = dex_strings.back();
    dex_strings.pop_back();
    auto valstr = val->c_str();
    auto vallen = strlen(valstr);
    auto not_file_name = !maybe_file_name(valstr, vallen);
    auto no_bad_char = is_reasonable_string(valstr, vallen);
    auto not_seen_yet = !set.count(val);
    if (not_seen_yet && not_file_name && no_bad_char) {
      return val;
    }
  }
  return nullptr;
}

static void strip_src_strings(
  DexStoresVector& stores, const char* map_path, PassManager& mgr) {
  size_t shortened = 0;
  size_t string_savings = 0;
  std::unordered_map<DexString*, std::vector<DexString*>> global_src_strings;
  std::unordered_set<DexString*> shortened_used;
  for (auto& classes : DexStoreClassesIterator(stores)) {
    for (auto const& clazz : classes) {
      auto src_string = clazz->get_source_file();
      if (src_string) {
        // inserting actual source files into this set will cause them to not
        // get used --- as the whole point of this analysis is to substitute
        // source file strings
        shortened_used.insert(src_string);
      }
    }
  }

  for (auto& classes : DexStoreClassesIterator(stores)) {
    std::unordered_map<DexString*, DexString*> src_to_shortened;
    std::vector<DexString*> current_dex_strings;
    for (auto const& clazz : classes) {
      clazz->gather_strings(current_dex_strings);
    }
    sort_unique(current_dex_strings, compare_dexstrings);
    // reverse current_dex_strings vector, so that we prefer strings that will
    // get smaller indices
    std::reverse(std::begin(current_dex_strings),
                 std::end(current_dex_strings));

    for (auto const& clazz : classes) {
      auto src_string = clazz->get_source_file();
      if (!src_string) {
        continue;
      }
      DexString* shortened_src_string = nullptr;
      if (src_to_shortened.count(src_string) == 0) {
        shortened_src_string =
            get_suitable_string(shortened_used, current_dex_strings);
        if (!shortened_src_string) {
          opt_warn(UNSHORTENED_SRC_STRING, "%s\n", SHOW(src_string));
          shortened_src_string = src_string;
        } else {
          shortened++;
          string_savings += strlen(src_string->c_str());
        }
        src_to_shortened[src_string] = shortened_src_string;
        shortened_used.emplace(shortened_src_string);
        global_src_strings[src_string].push_back(shortened_src_string);
      } else {
        shortened_src_string = src_to_shortened[src_string];
      }
      clazz->set_source_file(shortened_src_string);
    }
  }

  TRACE(SHORTEN, 1, "src strings shortened %ld, %lu bytes saved\n", shortened,
      string_savings);

  mgr.incr_metric(METRIC_SHORTENED_STRINGS, shortened);
  mgr.incr_metric(METRIC_BYTES_SAVED, string_savings);

  // generate mapping
  FILE* fd = fopen(map_path, "w");
  if (fd == nullptr) {
    perror("Error writing mapping file");
    return;
  }

  for (auto it : global_src_strings) {
    auto desc_vector = it.second;
    sort_unique(desc_vector);
    fprintf(fd, "%s ->", it.first->c_str());
    for (auto str : desc_vector) {
      fprintf(fd, " %s,", str->c_str());
    }
    fprintf(fd, "\n");
  }
  fclose(fd);
}

void ShortenSrcStringsPass::run_pass(
    DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  m_filename_mappings = cfg.metafile(m_filename_mappings);
  strip_src_strings(stores, m_filename_mappings.c_str(), mgr);
}

static ShortenSrcStringsPass s_pass;
