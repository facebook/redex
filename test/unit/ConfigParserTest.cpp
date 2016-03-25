/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include <sstream>

#include "keeprules.h"

TEST(ConfigParserTest, empty) {
  const char* simple_cls = "Lcom/facebook/foofoo/barbar/SomeClass;";
  const int cls_len = strlen(simple_cls);
  // totally wrong pattern. Should fail on the o of org
  const char* pattern1 = "Lorg/somethingelse";
  const int pat1_len = strlen(pattern1);
  // partial match on prefix. Should return match
  const char* partial_match = "Lcom/facebook";
  const int partial_len = strlen(partial_match);
  // Missing L. No partial matches which are not prefixes
  const char* no_L_match = "com/facebook";
  const int no_L_len = strlen(no_L_match);
  // Mismatch on class name
  const char* one_star_mismatch = "Lcom/facebook/*/barbar/OtherClass";
  const int one_star_mlen = strlen(one_star_mismatch);
  // single star should match for single level of the pkg hierarchy
  const char* one_star_match = "Lcom/facebook/*/barbar/SomeClass";
  const int one_star_len = strlen(one_star_match);
  // single star can't match multiple levels of the pkg hierarchy
  const char* one_star_mismatch2 = "Lcom/facebook/*/OtherClass";
  const int one_star_mlen2 = strlen(one_star_mismatch2);
  // Mismatch on class name
  const char* two_star_mismatch = "Lcom/facebook/**/OtherClass";
  const int two_star_mlen = strlen(two_star_mismatch);
  // Two stars should match even with multiple levels of the pkg hierarchy
  const char* two_star_match = "Lcom/facebook/**/SomeClass";
  const int two_star_len = strlen(two_star_match);
  // Should match anything
  const char* three_star_match = "***";
  const int three_star_len = strlen(three_star_match);
  EXPECT_FALSE(type_matches(pattern1, simple_cls, pat1_len, cls_len));
  EXPECT_TRUE(type_matches(partial_match, simple_cls, partial_len, cls_len));
  EXPECT_FALSE(type_matches(no_L_match, simple_cls, no_L_len, cls_len));
  EXPECT_FALSE(type_matches(one_star_mismatch, simple_cls, one_star_mlen, cls_len));
  EXPECT_TRUE(type_matches(one_star_match, simple_cls, one_star_len, cls_len));
  EXPECT_FALSE(type_matches(one_star_mismatch2, simple_cls, one_star_mlen2, cls_len));
  EXPECT_FALSE(type_matches(two_star_mismatch, simple_cls, two_star_mlen, cls_len));
  EXPECT_TRUE(type_matches(two_star_match, simple_cls, two_star_len, cls_len));
  EXPECT_TRUE(type_matches(three_star_match, simple_cls, three_star_len, cls_len));
}
