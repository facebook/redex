/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include <boost/regex.hpp>

#include "ProguardRegex.h"

using namespace redex;

TEST(ProguardRegexTest, members) {
  { boost::regex matcher("alpha");
    ASSERT_FALSE(boost::regex_match("pha", matcher));
    ASSERT_TRUE(boost::regex_match("alpha", matcher));
  }

  { // A ProGuard * should get translatd to a .*
    auto proguard_regex = "*";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    ASSERT_EQ(".*", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("pha", matcher));
    ASSERT_TRUE(boost::regex_match("alpha", matcher));
    ASSERT_TRUE(boost::regex_match("alpha54beta", matcher));
  }

  { // A ProGuard *alpha should get translated to .*pha
    auto proguard_regex = "*pha";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    ASSERT_EQ(".*pha", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("alpha", matcher));
    ASSERT_TRUE(boost::regex_match("betapha", matcher));
    ASSERT_FALSE(boost::regex_match("betapha42", matcher));
    ASSERT_TRUE(boost::regex_match("pha", matcher));
    ASSERT_FALSE(boost::regex_match("pha1066", matcher));
    ASSERT_FALSE(boost::regex_match("wombat", matcher));
  }

  { // Translate *pha* to .*pha.*
    auto proguard_regex = "*pha*";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    ASSERT_EQ(".*pha.*", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("alpha", matcher));
    ASSERT_TRUE(boost::regex_match("betapha", matcher));
    ASSERT_TRUE(boost::regex_match("betapha42", matcher));
    ASSERT_TRUE(boost::regex_match("pha", matcher));
    ASSERT_TRUE(boost::regex_match("pha1066", matcher));
    ASSERT_FALSE(boost::regex_match("wombat", matcher));
  }

  { // Translate wombat?numbat to wombat.numbat
    auto proguard_regex = "wombat?numbat";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    ASSERT_EQ("wombat.numbat", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("wombat.numbat", matcher));
    ASSERT_FALSE(boost::regex_match("wombat..numbat", matcher));
    ASSERT_FALSE(boost::regex_match("wombat", matcher));
    ASSERT_FALSE(boost::regex_match("numbat", matcher));
    ASSERT_TRUE(boost::regex_match("wombat1numbat", matcher));
  }

  { // Translate Wombat??Numbat to Wombat..Numbat
    auto proguard_regex = "Wombat??Numbat";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    ASSERT_EQ("Wombat..Numbat", r);
    boost::regex matcher(r);
    ASSERT_FALSE(boost::regex_match("WombatNumbat", matcher));
    ASSERT_FALSE(boost::regex_match("Wombat5Numbat", matcher));
    ASSERT_TRUE(boost::regex_match("Wombat55Numbat", matcher));
  }
}

TEST(ProguardRegexTest, types) {
  { // Translate % to (?:B|S|I|J|Z|F|D|C)
    auto proguard_regex = "%";
    auto r = proguard_parser::form_type_regex(proguard_regex);
    ASSERT_EQ("(?:B|S|I|J|Z|F|D|C|V)", r);
    boost::regex matcher(r);
    boost::cmatch m;
    ASSERT_TRUE(boost::regex_match("B", m, matcher));
    // Make sure we did not capture the group.
    ASSERT_EQ(1, m.size());
    ASSERT_TRUE(boost::regex_match("S", matcher));
    ASSERT_TRUE(boost::regex_match("I", matcher));
    ASSERT_TRUE(boost::regex_match("J", matcher));
    ASSERT_FALSE(boost::regex_match("int", matcher));
    ASSERT_TRUE(boost::regex_match("Z", matcher));
    ASSERT_TRUE(boost::regex_match("F", matcher));
    ASSERT_TRUE(boost::regex_match("D", matcher));
    ASSERT_TRUE(boost::regex_match("C", matcher));
    ASSERT_FALSE(boost::regex_match("void", matcher));
  }

  { auto proguard_regex = "com.*.redex.test.proguard.Delta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("Lcom\\/(?:[^\\/]*)\\/redex\\/test\\/proguard\\/Delta;", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    ASSERT_EQ(1, m.size());
  }

  { auto proguard_regex = "com.*.redex.*.proguard.Delta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("Lcom\\/(?:[^\\/]*)\\/redex\\/(?:[^\\/]*)\\/proguard\\/Delta;",
              r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    ASSERT_EQ(1, m.size());

    // Match against the first * but not the second *
    ASSERT_FALSE(boost::regex_match("Lcom/facebook/redex/", m, matcher));
  }

  { // Test matching using ** to match agaist a package name with any
    // number of separators.
    auto proguard_regex = "com.**.proguard.Delta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    ASSERT_EQ(1, m.size());
  }

  { auto proguard_regex = "com.**.proguard.**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    ASSERT_EQ(1, m.size());
  }

  { // The ? symbol should match any character in a class type except
    // the class separator symbol.
    auto proguard_regex = "com.alpha?beta.gamma";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    ASSERT_EQ("Lcom\\/alpha[^\\/]beta\\/gamma;", r);
    ASSERT_TRUE(boost::regex_match("Lcom/alpha1beta/gamma;", matcher));
    ASSERT_FALSE(boost::regex_match("Lcom/alphabeta/gamma;", matcher));
    ASSERT_FALSE(boost::regex_match("Lcom/alpha12beta/gamma;", matcher));
    ASSERT_FALSE(boost::regex_match("Lcom/alpha/beta/gamma;", matcher));
  }

  { // Make sure ** does not match primitive types or array types.
    auto proguard_regex = "**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("L(?:[^\\/]+(?:\\/[^\\/]+)*);", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("Ljava/lang/String;", matcher));
    ASSERT_FALSE(boost::regex_match("I", matcher));
    ASSERT_FALSE(boost::regex_match("[I", matcher));
    ASSERT_FALSE(boost::regex_match("[Ljava/util/List;", matcher));
  }

  { // Make sure ** works with array types.
    auto proguard_regex = "**[]";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("\\[L(?:[^\\/]+(?:\\/[^\\/]+)*);", r);
    boost::regex matcher(r);
    ASSERT_FALSE(boost::regex_match("Ljava/lang/String;", matcher));
    ASSERT_FALSE(boost::regex_match("I", matcher));
    ASSERT_FALSE(boost::regex_match("[I", matcher));
    ASSERT_TRUE(boost::regex_match("[Ljava/util/List;", matcher));
    ASSERT_FALSE(boost::regex_match("[[Ljava/util/List;", matcher));
  }

  { // Make sure ** works with multiple array types.
    auto proguard_regex = "java.**[][]";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("\\[\\[Ljava\\/(?:[^\\/]+(?:\\/[^\\/]+)*);", r);
    boost::regex matcher(r);
    ASSERT_FALSE(boost::regex_match("Ljava/lang/String;", matcher));
    ASSERT_FALSE(boost::regex_match("I", matcher));
    ASSERT_FALSE(boost::regex_match("[I", matcher));
    ASSERT_FALSE(boost::regex_match("[Ljava/util/List;", matcher));
    ASSERT_TRUE(boost::regex_match("[[Ljava/util/List;", matcher));
  }

  { // Make sure *** matches any type.
    auto proguard_regex = "***";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("\\[*(?:(?:B|S|I|J|Z|F|D|C|V)|L.*;)", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("Ljava/lang/String;", matcher));
    ASSERT_TRUE(boost::regex_match("I", matcher));
    ASSERT_TRUE(boost::regex_match("[I", matcher));
    ASSERT_TRUE(boost::regex_match("[Ljava/util/List;", matcher));
  }

  { // Check handling of ...
    auto proguard_regex = "...";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    ASSERT_EQ("(?:\\[*(?:(?:B|S|I|J|Z|F|D|C)|L.*;))*", r);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("Ljava/lang/String;", matcher));
    ASSERT_TRUE(boost::regex_match("I", matcher));
    ASSERT_TRUE(boost::regex_match("I[ILjava/lang/String;S", matcher));
    ASSERT_TRUE(boost::regex_match("Ljava/util/List;IZ", matcher));
    ASSERT_FALSE(boost::regex_match("(Ljava/util/List;IZ)I", matcher));
  }

  { // Check matching of nesting class types using $.
    auto proguard_regex = "com.facebook.redex.test.proguard.Delta$B";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    ASSERT_TRUE(boost::regex_match("Lcom/facebook/redex/test/proguard/Delta$B;", matcher));
  }

  // Check convert_wildcard_type
  {
    auto proguard_regex = "**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    ASSERT_EQ("L**;", descriptor);
  }
  {
    auto proguard_regex = "alpha.**.beta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    ASSERT_EQ("Lalpha/**/beta;", descriptor);
  }
  {
    auto proguard_regex = "alpha.**.beta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    ASSERT_EQ("Lalpha/**/beta;", descriptor);
  }
}
