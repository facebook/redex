/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

/*
 * Quickly concatenate std::strings
 *
 * These classes are necessary because std::ostringstream is slow to construct
 * and repeated calls to std::string::operator+= without a prior reserve
 * requires expensive re-allocs.
 *
 * StaticStringBuilder is for a compile time known number of strings
 * DynamicStringBuilder is for an arbitrary number of strings
 * (with a runtime hint)
 *
 * In practice, these seem to be fastest on a small number of strings. Too many,
 * and the memory cost of storing them degrades performance. When concatenating
 * a large unknown number of strings, it's probably faster to guess the final
 * size with string::reserve, then use std::string::operator+= repeatedly.
 *
 * Keep in mind that operator<< takes ownership of the given string
 */

namespace string_builders {

template <uint32_t num_strings>
class StaticStringBuilder {
 public:
  // Take ownership of s
  StaticStringBuilder& operator<<(std::string&& s) {
    always_assert(m_index < num_strings);
    m_total_chars += s.size();
    m_strings[m_index] = std::move(s);
    ++m_index;
    return *this;
  }

  std::string str() const {
    std::string result;
    result.reserve(m_total_chars);
    for (uint32_t i = 0; i < m_index; ++i) {
      result += m_strings[i];
    }
    return result;
  }

 private:
  uint32_t m_total_chars = 0;
  uint32_t m_index = 0;
  std::string m_strings[num_strings];
};

class DynamicStringBuilder {
 public:
  explicit DynamicStringBuilder(uint32_t expected_num_strings) {
    m_strings.reserve(expected_num_strings);
  }

  // Take ownership of s
  DynamicStringBuilder& operator<<(std::string&& s) {
    m_total_chars += s.size();
    m_strings.push_back(std::move(s));
    return *this;
  }

  std::string str() const {
    std::string result;
    result.reserve(m_total_chars);
    for (const auto& s : m_strings) {
      result += s;
    }
    return result;
  }

 private:
  uint32_t m_total_chars = 0;
  std::vector<std::string> m_strings;
};

} // namespace string_builders
