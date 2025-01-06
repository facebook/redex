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

namespace dex::loader::details {
struct Accessor;
} // namespace dex::loader::details

class DexLoader {
 public:
  using DataUPtr =
      std::unique_ptr<const uint8_t, std::function<void(const uint8_t*)>>;

 private:
  const dex_header* m_dh;
  std::unique_ptr<DexIdx> m_idx;
  const dex_class_def* m_class_defs;
  DexClasses* m_classes;
  DataUPtr m_data;
  size_t m_file_size;
  const DexLocation* m_location;

 public:
  enum class Parallel { kYes, kNo };

  static DexLoader create(const DexLocation* location,
                          DataUPtr data,
                          size_t size);

  DexIdx* get_idx() { return m_idx.get(); }

 private:
  explicit DexLoader(const DexLocation* location, DataUPtr data, size_t size);

  DexClasses load_dex(dex_stats_t* stats,
                      int support_dex_version,
                      Parallel p = Parallel::kYes);
  DexClasses load_dex(dex_stats_t* stats, Parallel p = Parallel::kYes);

  void load_dex_class(int num);

  void gather_input_stats(dex_stats_t* stats, const dex_header* dh);

  friend struct dex::loader::details::Accessor;
  friend struct Dex038TestAccessor;
  friend struct Dex039TestAccessor;
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
