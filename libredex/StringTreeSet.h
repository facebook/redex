/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <sstream>
#include <string>

// The StringTreeSet provides a compact encoding for a set of strings that tend
// to share prefices.
class StringTreeSet {
 public:
  void insert(const std::string& s, size_t start = 0);

  void encode(std::ostringstream& oss) const;

 private:
  std::map<char, StringTreeSet> m_map;
  bool m_terminal{false};
};
