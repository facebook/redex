/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexDefs.h"
#include "DexIdx.h"
#include "DexStats.h"
#include "DexUtil.h"

class DexLoader {
 public:
  using DataUPtr =
      std::unique_ptr<const uint8_t, std::function<void(const uint8_t*)>>;

  enum class Parallel { kYes, kNo };

 private:
  const dex_header* m_dh;
  std::unique_ptr<DexIdx> m_idx;
  const dex_class_def* m_class_defs;
  DexClasses m_classes;
  DataUPtr m_data;
  size_t m_file_size;
  const DexLocation* m_location;
  dex_stats_t m_stats{};
  int m_support_dex_version;
  Parallel m_parallel;

 public:
  static DexLoader create(const DexLocation* location,
                          DataUPtr data,
                          size_t size,
                          int support_dex_version = 35,
                          Parallel parallel = Parallel::kYes);

  // Convenience API to load from file.
  static DexLoader create(const DexLocation* location,
                          int support_dex_version = 35,
                          Parallel parallel = Parallel::kYes);

  DexClasses& get_classes() { return m_classes; }
  DexIdx* get_idx() { return m_idx.get(); }
  dex_stats_t& get_stats() { return m_stats; }

 private:
  explicit DexLoader(const DexLocation* location,
                     DataUPtr data,
                     size_t size,
                     int support_dex_version,
                     Parallel parallel);

  void load_dex();

  void load_dex_class(int num);

  void gather_input_stats();
};

DexClasses load_classes_from_dex(
    const DexLocation* location,
    dex_stats_t* stats = nullptr,
    bool balloon = true,
    bool throw_on_balloon_error = true,
    int support_dex_version = 35,
    DexLoader::Parallel p = DexLoader::Parallel::kYes);
DexClasses load_classes_from_dex(
    const dex_header* dh,
    const DexLocation* location,
    bool balloon = true,
    bool throw_on_balloon_error = true,
    DexLoader::Parallel p = DexLoader::Parallel::kYes);
DexClasses load_classes_from_dex(
    DexLoader::DataUPtr data,
    size_t data_size,
    const DexLocation* location,
    bool balloon = true,
    bool throw_on_balloon_error = true,
    int support_dex_version = 35,
    DexLoader::Parallel p = DexLoader::Parallel::kYes);

std::string load_dex_magic_from_dex(const DexLocation* location);
void balloon_for_test(const Scope& scope);

static inline const uint8_t* align_ptr(const uint8_t* const ptr,
                                       const size_t alignment) {
  const size_t alignment_error = ((size_t)ptr) % alignment;
  if (alignment_error != 0) {
    return ptr + alignment - alignment_error;
  } else {
    return ptr;
  }
}
