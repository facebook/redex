/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include <cstring>
#include <string>

// This could be faster with DP, but this code is simpler to read and it's
// fast enough with reasonable inputs.  Just don't call
// pattern_match('**************************', 'aaaaaaaaaaaaaaaaaaaaaaaaa')...
bool pattern_match(const char* pattern, const char* name, size_t pl, int nl) {
  if (*pattern == '*') {
    bool step2 = false, step1 = false;
    if (pl > 1 && *(pattern + 1) == '*') {
      // Double star means .* aka match anything, including across package names.
      // make sure we don't walk off the end of either string
      if (pl > 2) {
        step2 = pattern_match(pattern + 2, name, pl - 2, nl);
      }
      if (nl > 1) {
        step1 = pattern_match(pattern, name + 1, pl, nl - 1);
      }
      return (step2 || step1);
    } else {
      // Single star means [^.]*  aka match a sequence of any length of
      // non-package separator characters.
      bool step1 = false, step2 = false;
      if (pl > 1) {
        step1 = pattern_match(pattern + 1, name, pl - 1, nl);
      }
      if (nl > 1) {
        step2 = pattern_match(pattern, name + 1, pl, nl - 1);
      }
      return (step1 || (*name != '/' && step2));
    }
  } else if (*pattern == '\0') {
    return *name == '\0';
  } else if (*pattern == *name) {
    // If we arrive at the end of the pattern but aren't yet
    // at the end of the class name we mark this is a valid match.
    // This means the rule Lcom/blah will match against Lcom/blah/Foo
    bool step = true;
    if (pl > 1 && nl > 1) {
      step = pattern_match(pattern + 1, name + 1, pl - 1, nl - 1);
    }
    return step;
  } else {
    return false;
  }
}


bool type_matches(const char* pattern, const char* name, size_t pl, size_t nl) {
  if (pattern == nullptr ||
      strcmp(pattern, "*") == 0 ||
      strcmp(pattern, "***") == 0) {
    return true;
  }
  return pattern_match(pattern, name, pl, nl);
}
