/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

inline std::string str_copy(std::string_view str) { return std::string(str); }

inline std::string operator+(const std::string_view s,
                             const std::string_view t) {
  auto tmp = str_copy(s);
  tmp.append(t);
  return tmp;
}

inline std::string operator+(const char* s, const std::string_view t) {
  std::string tmp(s);
  tmp.append(t);
  return tmp;
}

inline std::string operator+(const std::string_view s, const char* t) {
  auto tmp = str_copy(s);
  tmp.append(t);
  return tmp;
}

inline std::string operator+(char s, const std::string_view t) {
  std::string tmp(1, s);
  tmp.append(t);
  return tmp;
}

inline std::string operator+(const std::string_view s, char t) {
  auto tmp = str_copy(s);
  tmp.append(1, t);
  return tmp;
}

class StringStorage {
 private:
  std::unordered_set<std::string_view> m_set;
  std::vector<std::unique_ptr<char[]>> m_storage;

 public:
  std::string_view operator[](std::string_view str) {
    auto it = m_set.find(str);
    if (it == m_set.end()) {
      auto data = std::make_unique<char[]>(str.size());
      auto data_ptr = data.get();
      memcpy(data_ptr, str.data(), str.size());
      m_storage.push_back(std::move(data));
      it = m_set.emplace(std::string_view(data_ptr, str.size())).first;
    }
    return *it;
  }
};
