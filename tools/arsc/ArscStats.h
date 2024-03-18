/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "androidfw/ResourceTypes.h"

namespace attribution {
//
// Helper functions that are exposed to be easily testable.
//

// Returns the number of bytes used to pad character data to 4 byte alignment.
size_t count_padding(const android::ResStringPool_header* header,
                     const android::ResStringPool& pool);
// Returns the number of bytes used to encode the length of the string,
// the string characters, and the null zero.
size_t compute_string_character_size(const android::ResStringPool& pool,
                                     uint32_t idx);
// Returns the number of bytes used to represent a string in the pool as used by
// a resource reference, which will count the bytes for the offset and chase
// down spans and count that up too.
size_t compute_string_size(const android::ResStringPool& pool, uint32_t idx);

//
// API for callers follows.
//

struct ResourceSize {
  // Number of bytes that exist in the arsc file only because of this single
  // resource id.
  size_t private_size = 0;
  // Number of bytes that represent this resource (its name/value) and some
  // other resource(s). Deduplication is and name obfuscation is what
  // contributes to this.
  size_t shared_size = 0;
  // The amount of space in the file divided by the number of other resource ids
  // that are responsible for the bytes.
  double proportional_size = 0;
};

// Represents all computed data, for formatting/presenting in another format by
// caller.
struct Result {
  uint32_t id;
  std::string type;
  std::string name;
  ResourceSize sizes;
  std::vector<std::string> configs;
};

using ResourceNames = std::unordered_map<uint32_t, std::string>;

class ArscStats {
 public:
  ArscStats(const void* arsc_data,
            size_t file_len,
            const ResourceNames& resid_to_name)
      : m_data(arsc_data),
        m_file_len(file_len),
        m_given_resid_to_name(resid_to_name) {}

  std::vector<Result> compute();

 private:
  const void* m_data;
  size_t m_file_len;
  const ResourceNames& m_given_resid_to_name;
};

} // namespace attribution
