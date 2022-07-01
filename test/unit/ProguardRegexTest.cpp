/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/regex.hpp>

#include "ProguardRegex.h"

using namespace keep_rules;

TEST(ProguardRegexTest, members) {
  {
    boost::regex matcher("alpha");
    EXPECT_FALSE(boost::regex_match("pha", matcher));
    EXPECT_TRUE(boost::regex_match("alpha", matcher));
  }

  { // A ProGuard * should get translatd to a .*
    auto proguard_regex = "*";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    EXPECT_EQ(".*", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("pha", matcher));
    EXPECT_TRUE(boost::regex_match("alpha", matcher));
    EXPECT_TRUE(boost::regex_match("alpha54beta", matcher));
  }

  { // A ProGuard *alpha should get translated to .*pha
    auto proguard_regex = "*pha";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    EXPECT_EQ(".*pha", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("alpha", matcher));
    EXPECT_TRUE(boost::regex_match("betapha", matcher));
    EXPECT_FALSE(boost::regex_match("betapha42", matcher));
    EXPECT_TRUE(boost::regex_match("pha", matcher));
    EXPECT_FALSE(boost::regex_match("pha1066", matcher));
    EXPECT_FALSE(boost::regex_match("wombat", matcher));
  }

  { // Translate *pha* to .*pha.*
    auto proguard_regex = "*pha*";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    EXPECT_EQ(".*pha.*", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("alpha", matcher));
    EXPECT_TRUE(boost::regex_match("betapha", matcher));
    EXPECT_TRUE(boost::regex_match("betapha42", matcher));
    EXPECT_TRUE(boost::regex_match("pha", matcher));
    EXPECT_TRUE(boost::regex_match("pha1066", matcher));
    EXPECT_FALSE(boost::regex_match("wombat", matcher));
  }

  { // Translate wombat?numbat to wombat.numbat
    auto proguard_regex = "wombat?numbat";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    EXPECT_EQ("wombat.numbat", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("wombat.numbat", matcher));
    EXPECT_FALSE(boost::regex_match("wombat..numbat", matcher));
    EXPECT_FALSE(boost::regex_match("wombat", matcher));
    EXPECT_FALSE(boost::regex_match("numbat", matcher));
    EXPECT_TRUE(boost::regex_match("wombat1numbat", matcher));
  }

  { // Translate Wombat??Numbat to Wombat..Numbat
    auto proguard_regex = "Wombat??Numbat";
    auto r = proguard_parser::form_member_regex(proguard_regex);
    EXPECT_EQ("Wombat..Numbat", r);
    boost::regex matcher(r);
    EXPECT_FALSE(boost::regex_match("WombatNumbat", matcher));
    EXPECT_FALSE(boost::regex_match("Wombat5Numbat", matcher));
    EXPECT_TRUE(boost::regex_match("Wombat55Numbat", matcher));
  }
}

TEST(ProguardRegexTest, types) {
  { // Translate % to (?:B|S|I|J|Z|F|D|C)
    auto proguard_regex = "%";
    auto r = proguard_parser::form_type_regex(proguard_regex);
    EXPECT_EQ("(?:B|S|I|J|Z|F|D|C|V)", r);
    boost::regex matcher(r);
    boost::cmatch m;
    EXPECT_TRUE(boost::regex_match("B", m, matcher));
    // Make sure we did not capture the group.
    EXPECT_EQ(1, m.size());
    EXPECT_TRUE(boost::regex_match("S", matcher));
    EXPECT_TRUE(boost::regex_match("I", matcher));
    EXPECT_TRUE(boost::regex_match("J", matcher));
    EXPECT_FALSE(boost::regex_match("int", matcher));
    EXPECT_TRUE(boost::regex_match("Z", matcher));
    EXPECT_TRUE(boost::regex_match("F", matcher));
    EXPECT_TRUE(boost::regex_match("D", matcher));
    EXPECT_TRUE(boost::regex_match("C", matcher));
    EXPECT_FALSE(boost::regex_match("void", matcher));
  }

  {
    std::string_view proguard_regex = "com.*.redex.test.proguard.Delta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ("Lcom\\/(?:[^\\/\\[]*)\\/redex\\/test\\/proguard\\/Delta;", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    EXPECT_EQ(1, m.size());
  }

  {
    std::string_view proguard_regex = "com.*.redex.*.proguard.Delta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ(
        "Lcom\\/(?:[^\\/\\[]*)\\/redex\\/(?:[^\\/\\[]*)\\/proguard\\/Delta;",
        r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    EXPECT_EQ(1, m.size());

    // Match against the first * but not the second *
    EXPECT_FALSE(boost::regex_match("Lcom/facebook/redex/", m, matcher));
  }

  { // Test matching using ** to match agaist a package name with any
    // number of separators.
    std::string_view proguard_regex = "com.**.proguard.Delta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    EXPECT_EQ(1, m.size());
  }

  {
    std::string_view proguard_regex = "com.**.proguard.**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    EXPECT_EQ(1, m.size());
  }

  {
    std::string_view proguard_regex = "**proguard**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    boost::cmatch m;
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard/Delta;", m, matcher));
    EXPECT_EQ(1, m.size());
    EXPECT_TRUE(boost::regex_match(
        "Lcom/facebook/redex/test/proguard_Delta;", m, matcher));
    EXPECT_EQ(1, m.size());
    EXPECT_TRUE(boost::regex_match("Lproguard_Delta;", m, matcher));
    EXPECT_EQ(1, m.size());
  }

  { // The ? symbol should match any character in a class type except
    // the class separator symbol.
    std::string_view proguard_regex = "com.alpha?beta.gamma";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    EXPECT_EQ("Lcom\\/alpha[^\\/\\[]beta\\/gamma;", r);
    EXPECT_TRUE(boost::regex_match("Lcom/alpha1beta/gamma;", matcher));
    EXPECT_FALSE(boost::regex_match("Lcom/alphabeta/gamma;", matcher));
    EXPECT_FALSE(boost::regex_match("Lcom/alpha12beta/gamma;", matcher));
    EXPECT_FALSE(boost::regex_match("Lcom/alpha/beta/gamma;", matcher));
  }

  { // Make sure ** does not match primitive types or array types.
    std::string_view proguard_regex = "**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ("L(?:[^\\[]*);", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("Ljava/lang/String;", matcher));
    EXPECT_FALSE(boost::regex_match("I", matcher));
    EXPECT_FALSE(boost::regex_match("[I", matcher));
    EXPECT_FALSE(boost::regex_match("[Ljava/util/List;", matcher));
  }

  { // Make sure ** works with array types.
    std::string_view proguard_regex = "**[]";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ("\\[L(?:[^\\[]*);", r);
    boost::regex matcher(r);
    EXPECT_FALSE(boost::regex_match("Ljava/lang/String;", matcher));
    EXPECT_FALSE(boost::regex_match("I", matcher));
    EXPECT_FALSE(boost::regex_match("[I", matcher));
    EXPECT_TRUE(boost::regex_match("[Ljava/util/List;", matcher));
    EXPECT_FALSE(boost::regex_match("[[Ljava/util/List;", matcher));
  }

  { // Make sure ** works with multiple array types.
    std::string_view proguard_regex = "java.**[][]";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ("\\[\\[Ljava\\/(?:[^\\[]*);", r);
    boost::regex matcher(r);
    EXPECT_FALSE(boost::regex_match("Ljava/lang/String;", matcher));
    EXPECT_FALSE(boost::regex_match("I", matcher));
    EXPECT_FALSE(boost::regex_match("[I", matcher));
    EXPECT_FALSE(boost::regex_match("[Ljava/util/List;", matcher));
    EXPECT_TRUE(boost::regex_match("[[Ljava/util/List;", matcher));
  }

  { // Make sure *** matches any type.
    std::string_view proguard_regex = "***";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ("\\[*(?:(?:B|S|I|J|Z|F|D|C|V)|L.*;)", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("Ljava/lang/String;", matcher));
    EXPECT_TRUE(boost::regex_match("I", matcher));
    EXPECT_TRUE(boost::regex_match("[I", matcher));
    EXPECT_TRUE(boost::regex_match("[Ljava/util/List;", matcher));
  }

  { // Check handling of ...
    std::string_view proguard_regex = "...";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    EXPECT_EQ("(?:\\[*(?:(?:B|S|I|J|Z|F|D|C)|L.*;))*", r);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("Ljava/lang/String;", matcher));
    EXPECT_TRUE(boost::regex_match("I", matcher));
    EXPECT_TRUE(boost::regex_match("I[ILjava/lang/String;S", matcher));
    EXPECT_TRUE(boost::regex_match("Ljava/util/List;IZ", matcher));
    EXPECT_FALSE(boost::regex_match("(Ljava/util/List;IZ)I", matcher));
  }

  { // Check matching of nesting class types using $.
    std::string_view proguard_regex =
        "com.facebook.redex.test.proguard.Delta$B";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    auto r = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(r);
    EXPECT_TRUE(boost::regex_match("Lcom/facebook/redex/test/proguard/Delta$B;",
                                   matcher));
  }

  // Check convert_wildcard_type
  {
    std::string_view proguard_regex = "**";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    EXPECT_EQ("L**;", descriptor);
  }
  {
    std::string_view proguard_regex = "alpha.**.beta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    EXPECT_EQ("Lalpha/**/beta;", descriptor);
  }
  {
    std::string_view proguard_regex = "alpha.**.beta";
    auto descriptor = proguard_parser::convert_wildcard_type(proguard_regex);
    EXPECT_EQ("Lalpha/**/beta;", descriptor);
  }
}
