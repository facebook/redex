/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringTreeSet.h"

#include "Debug.h"
#include <unordered_map>

void StringTreeSet::insert(const std::string& s, size_t start) {
  if (start == s.size()) {
    m_terminal = true;
    return;
  }
  always_assert(start < s.size());
  m_map[s.at(start)].insert(s, start + 1);
}

void StringTreeSet::encode(std::ostringstream& oss) const {
  if (!m_terminal && m_map.size() == 1) {
    auto&& [c, rest] = *m_map.begin();
    always_assert(c >= 32);
    oss.put(c);
    rest.encode(oss);
    return;
  }
  always_assert(m_terminal || !m_map.empty());
  always_assert(m_map.size() < 64);
  size_t terminal_and_size = m_terminal | (m_map.size() << 1);
  always_assert(terminal_and_size > 0);
  always_assert(terminal_and_size < 128);
  if (terminal_and_size >= 31) {
    oss.put(31);
  }
  oss.put(terminal_and_size);
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

std::string StringTreeSet::encode_string_tree_set(
    const std::vector<std::string>& strings) {
  StringTreeSet sts;
  for (const auto& s : strings) {
    sts.insert(s);
  }
  std::ostringstream oss;
  sts.encode(oss);
  return oss.str();
}
