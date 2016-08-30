/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ProguardRegex.h"

namespace redex {
namespace proguard_parser {

// Convert a ProGuard member regex to a boost::regex
// Example: "alpha*beta?gamma" -> "alpha.*beta.gamma"
std::string form_member_regex(std::string proguard_regex) {
  std::string r;
  for (const char ch : proguard_regex) {
    // A * matches any part of a field or method name. Convert this
    // into the regex .*
    if (ch == '*') {
      r += ".";
    }
    // A ? matches any single character in a field or method name.
    // Convert this into the regex . and discard the ?
    if (ch == '?') {
      r += '.';
      continue;
    }
    r += ch;
  }
  return r;
}

// Convert a ProGuard type regex to a boost::regex
// Example: "%" -> "(?:B|S|I|J|Z|F|D|C)"
// Example: "alpha?beta" -> "Lalpha.beta"
// Example: "alpha/*/beta" -> "Lalpha\\/([^\\/]+)\\/beta"
// Example: "alpha/**/beta" ->  "Lalpha\\/([^\\/]+(?:\\/[^\\/]+)*)\\/beta"
std::string form_type_regex(std::string proguard_regex) {
  // Convert % to a match against primvitive types without
  // creating a capture group.
  if (proguard_regex == "%") {
    return "(?:B|S|I|J|Z|F|D|C)";
  }
  std::string r;
  for (size_t i = 0; i < proguard_regex.size(); i++) {
    const char ch = proguard_regex[i];
    // Convert the . to a slash
    if (ch == '.') {
      r += "\\/";
      continue;
    }
    // ? should match any character except the class seperator.
    if (ch == '?') {
      r += "[^\\/]";
      continue;
    }
    if (ch == '*') {
      if ((i != proguard_regex.size()-1) && (proguard_regex[i+1] == '*')) {
        r += "([^\\/]+(?:\\/[^\\/]+)*)";
        i++;
        continue;
      }
      r += "([^\\/]+)";
      continue;
    }
    r += ch;
  }
  return "L" + r;
}

} // namespace proguard_parser
} // namespace redex
