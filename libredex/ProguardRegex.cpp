/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstring>

#include "ProguardRegex.h"
#include "ProguardMap.h"

namespace redex {
namespace proguard_parser {

// Convert a ProGuard member regex to a boost::regex
// Example: "alpha*beta?gamma" -> "alpha.*beta.gamma"
std::string form_member_regex(std::string proguard_regex) {
  // An empty string matches against any member name.
  if (proguard_regex.empty()) {
    return ".*";
  }
  std::string r;
  r.reserve(proguard_regex.size());
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
//
// See this link for more details
// www.guardsquare.com/en/products/proguard/manual/usage#classspecification
//
// Example: "%" -> "(?:B|S|I|J|Z|F|D|C|V)"
// Example: "Lalpha?beta;" -> "Lalpha[^\\/\\[]beta;"
// Example: "Lalpha/*/beta;" -> "Lalpha\\/(?:[^\\/\\[]*)\\/beta;"
// Example: "Lalpha/**/beta;" ->  "Lalpha\\/(?:[^\\[]*)\\/beta;"
std::string form_type_regex(std::string proguard_regex) {
  if (proguard_regex.empty()) {
    return ".*";
  }
  if (proguard_regex == "L*;") {
    proguard_regex = "L**;";
  }
  std::string r;
  r.reserve(2 * proguard_regex.size());
  for (size_t i = 0; i < proguard_regex.size(); i++) {
    const char ch = proguard_regex[i];
    // Convert % to a match against primvitive types without
    // creating a capture group.
    if (ch == '%') {
      r += "(?:B|S|I|J|Z|F|D|C|V)";
      continue;
    }
    // Escape the $ character
    if (ch == '$') {
      r += "\\$";
      continue;
    }
    // Escape a path slash to it is not part of the regex syntax.
    if (ch == '/') {
      r += "\\/";
      continue;
    }
    // Preserve brackets.
    if (ch == '(') {
      r += "\\(";
      continue;
    }
    if (ch == ')') {
      r += "\\)";
      continue;
    }
    // Escape an array [ so it is not part of the regex syntax.
    if (ch == '[') {
      r += "\\[";
      continue;
    }
    // ?: match any character except the class seperator or array prefix
    if (ch == '?') {
      r += "[^\\/\\[]";
      continue;
    }
    if (ch == '*') {
      if ((i != proguard_regex.size() - 1) && (proguard_regex[i + 1] == '*')) {
        if ((i != proguard_regex.size() - 2) &&
            (proguard_regex[i + 2] == '*')) {
          // ***: Match any single type i.e. a primitive type or a class type.
          r += "\\[*(?:(?:B|S|I|J|Z|F|D|C|V)|L.*;)";
          i = i + 2;
          continue;
        }
        // **: Match any part of a class name including any number of seperators
        // Note that this does not match an array type
        r += "(?:[^\\[]*)";
        i++;
        continue;
      }
      // *: Match any part of a class name not containing the package separator
      // Note that this does not match an array type
      r += "(?:[^\\/\\[]*)";
      continue;
    }
    if (ch == '.') {
      if ((i != proguard_regex.size() - 1) && (proguard_regex[i + 1] == '.')) {
        if ((i != proguard_regex.size() - 2) &&
            (proguard_regex[i + 2] == '.')) {
          // ...: Match any sequence of types.
          r += "(?:\\[*(?:(?:B|S|I|J|Z|F|D|C)|L.*;))*";
          i = i + 2;
          continue;
        }
      }
    }
    r += ch;
  }
  return r;
}

// Convert a ProGuard Java type type which may use wildcards to
// an internal JVM type descriptor with the wildcards preserved.
std::string convert_wildcard_type(std::string typ) {
  redex_assert(!typ.empty());
  const std::string& desc = convert_type(typ);
  // Fix up the descriptor to move Ls that occur before wildcards.
  std::string wildcard_descriptor;
  wildcard_descriptor.reserve(desc.size());
  bool supress_semicolon = false;
  bool keep_dots = false;
  for (unsigned int i = 0; i < desc.size(); i++) {
    if (desc[i] == 'L') {
      if (desc[i + 1] == '%') {
        supress_semicolon = true;
        continue;
      }
      if (desc[i + 1] == '*' && desc.size() >= i + 2 && desc[i + 2] == '*' &&
          desc[i + 3] == '*') {
        supress_semicolon = true;
        continue;
      }
      if (desc[i + 1] == '/' && desc.size() >= i + 2 && desc[i + 2] == '/' &&
          desc[i + 3] == '/') {
        supress_semicolon = true;
        keep_dots = true;
        continue;
      }
    }
    if (desc[i] == '/' && keep_dots) {
      wildcard_descriptor += ".";
      continue;
    }
    if (desc[i] == ';' && supress_semicolon) {
      supress_semicolon = false;
      keep_dots = false;
      continue;
    }
    wildcard_descriptor += desc[i];
  }
  return wildcard_descriptor;
}

} // namespace proguard_parser
} // namespace redex
