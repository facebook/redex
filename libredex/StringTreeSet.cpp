/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringTreeSet.h"

#include <cmath>
#include <unordered_map>

#include "Debug.h"
#include "DexEncoding.h"

namespace {
constexpr uint8_t BITS_PER_PAYLOAD_UNIT = 6;
constexpr uint8_t FLAG_PAYLOAD_UNIT = 1 << BITS_PER_PAYLOAD_UNIT;
constexpr uint8_t PAYLOAD_MASK = FLAG_PAYLOAD_UNIT - 1;
constexpr uint8_t FLAG_NONTERMINAL = 1 << 4;
constexpr uint8_t FLAG_NO_PAYLOAD = 1 << 3;

template <typename ValueType>
size_t payload_unit_count(ValueType value) {
  size_t max =
      std::ceil((double)(sizeof(ValueType) * 8) / BITS_PER_PAYLOAD_UNIT);
  // NOLINTNEXTLINE(bugprone-branch-clone)
  if (value < 0) {
    return max;
  } else if (value == 0) {
    return 0;
  } else if (value < 64) {
    return 1;
  } else if (value < 4096) {
    return 2;
  } else if (value < 262144) {
    return 3;
  } else if (value < 16777216) {
    return 4;
  } else if (value < 1073741824) {
    return 5;
  } else {
    return max;
  }
}
} // namespace

template <typename ValueType>
void StringTreeMap<ValueType>::insert(const std::string& s,
                                      ValueType value,
                                      size_t start) {
  if (start == s.size()) {
    m_terminal = true;
    m_value = value;
    return;
  }
  always_assert(start < s.size());
  m_map[s.at(start)].insert(s, value, start + 1);
}

template <typename ValueType>
void StringTreeMap<ValueType>::encode(std::ostringstream& oss) const {
  if (!m_terminal && m_map.size() == 1) {
    auto&& [c, rest] = *m_map.begin();
    always_assert(c >= 32);
    oss.put(c);
    rest.encode(oss);
    return;
  }
  always_assert(m_terminal || !m_map.empty());
  // Write <= 31 to pack in the following:
  // A B C D E
  // A = non terminal?
  // B = payload is zero?
  // CDE = how many payload chars come next, each char will have 0x40 set and
  // use lowest 6 bits to denote the value.
  size_t num_payload_chars = payload_unit_count(m_value);
  always_assert(num_payload_chars < FLAG_NO_PAYLOAD);
  char header = m_terminal
                    ? (m_value == 0 ? FLAG_NO_PAYLOAD : num_payload_chars)
                    : FLAG_NONTERMINAL;
  oss.put(header);
  // Each payload char will be nonzero to make string encoding work (which will
  // need to get shifted and assembled to read proper value).
  if (m_terminal && m_value != 0) {
    uint64_t value_to_write = m_value & 0xFFFFFFFF;
    for (size_t i = 0; i < num_payload_chars; i++) {
      oss.put(FLAG_PAYLOAD_UNIT | (value_to_write & PAYLOAD_MASK));
      value_to_write = value_to_write >> BITS_PER_PAYLOAD_UNIT;
    }
  }
  // Followed by the size of this tree's map + 1.
  size_t map_size = m_map.size() + 1;
  always_assert(map_size < 128);
  oss.put(map_size);
  bool first{true};
  std::unordered_map<char, std::ostringstream::pos_type> offsets;
  for (auto&& [c, nested] : m_map) {
    oss.put(c);
    if (first) {
      first = false;
    } else {
      offsets.emplace(c, oss.tellp());
      oss.put(0);
      oss.put(0);
      oss.put(0);
    }
  }
  first = true;
  for (auto&& [c, rest] : m_map) {
    if (first) {
      first = false;
    } else {
      auto pos = oss.tellp();
      always_assert(pos < 127 * 127 * 127);
      oss.seekp(offsets.at(c));
      oss.put((pos % 127) + 1);
      oss.put(((pos / 127) % 127) + 1);
      oss.put((pos / (127 * 127)) + 1);
      oss.seekp(pos);
    }
    rest.encode(oss);
  }
}

template <typename ValueType>
std::string StringTreeMap<ValueType>::encode_string_tree_map(
    const std::map<std::string, ValueType>& strings) {
  StringTreeMap<ValueType> stm;
  for (const auto& [s, v] : strings) {
    stm.insert(s, v);
  }
  std::ostringstream oss;
  stm.encode(oss);
  return oss.str();
}

template class StringTreeMap<int16_t>;
template class StringTreeMap<int32_t>;

void StringTreeSet::encode(std::ostringstream& oss) const {
  StringTreeMap<int16_t> stm;
  for (const auto& s : m_set) {
    stm.insert(s, 0);
  }
  return stm.encode(oss);
}

std::string StringTreeSet::encode_string_tree_set(
    const std::vector<std::string>& strings) {
  StringTreeMap<int16_t> stm;
  for (const auto& s : strings) {
    stm.insert(s, 0);
  }
  std::ostringstream oss;
  stm.encode(oss);
  return oss.str();
}

std::string StringTreeStringMap::encode_string_tree_map(
    const std::map<std::string, std::string>& strings) {
  // pushes the base 127 encoding of an integer
  constexpr int32_t ENCODED_INT_SIZE = 3;
  auto push_int = [](int32_t value, std::ostringstream* dest) {
    always_assert(value < 127 * 127 * 127);
    auto& stream = *dest;
    stream.put((value % 127) + 1);
    stream.put(((value / 127) % 127) + 1);
    stream.put((value / (127 * 127)) + 1);
  };

  auto compute_utf16_length = [](const std::string& str) {
    auto s = str.data();
    int32_t result{0};
    while (*s != '\0') {
      auto code_point = mutf8_next_code_point(s);
      ++result;
      if (code_point >= 0x10000) {
        ++result;
      }
    }
    return result;
  };

  std::ostringstream pool;
  std::map<std::string, int32_t> value_to_offset;
  // NOTE: DO NOT use tellp() here to figure out the size and offsets! Offsets
  // must appear in terms of Java char counts, which will be UTF-16 code units.
  int32_t pool_size{0};
  for (auto&& [key, value] : strings) {
    // if a value already exists, its offset will be reused
    auto it = value_to_offset.try_emplace(value, pool_size);
    if (it.second) {
      auto length = compute_utf16_length(value);
      push_int(length, &pool);
      pool << value;
      pool_size += ENCODED_INT_SIZE + length;
    }
  }

  std::ostringstream tree;
  StringTreeMap<int32_t> stm;
  for (const auto& [key, value] : strings) {
    auto off = value_to_offset.at(value);
    stm.insert(key, off);
  }
  stm.encode(tree);

  // NOTE: tellp() OK here because tree will use only non-zero ascii chars. Be
  // sure to update this if the constraint ever changes.
  auto tree_size = (int32_t)tree.tellp();
  constexpr int32_t header_size{6};
  std::ostringstream oss;
  // Offset to string tree
  push_int(header_size, &oss);
  // Offset to pool
  push_int(header_size + tree_size, &oss);
  auto encoded_tree = tree.str();
  oss << encoded_tree;
  oss << pool.str();
  return oss.str();
}
