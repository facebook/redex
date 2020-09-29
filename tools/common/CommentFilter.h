/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/operations.hpp>
#include <cstdio>

class CommentFilter {
 public:
  using char_type = char;
  using category = boost::iostreams::input_filter_tag;

  template <typename Source>
  int get(Source& src) {
    int c = boost::iostreams::get(src);
    unsigned char c_char = (unsigned char)c;

    if (c_char == '\\' && !m_seen_backslash) {
      m_seen_backslash = true;
    } else if (c_char == '"' && !m_seen_backslash) {
      m_seen_backslash = false;
      m_in_quotes = !m_in_quotes;

    } else if (c_char == '#' && !m_in_quotes) {
      m_seen_backslash = false;
      for (c = boost::iostreams::get(src); c != EOF && (unsigned char)c != '\n';
           c = boost::iostreams::get(src))
        ;
    } else {
      m_seen_backslash = false;
    }

    return c;
  }

 private:
  bool m_in_quotes{false};
  bool m_seen_backslash{false};
};
