/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardPrintConfiguration.h"

#include <gtest/gtest.h>
#include <sstream>

#include "ProguardConfiguration.h"
#include "ProguardParser.h"

namespace {

// Parses one rule and prints it back out.
std::string round_trip(const std::string& rule) {
  std::istringstream input(rule);
  keep_rules::ProguardConfiguration config;
  keep_rules::proguard_parser::parse(input, &config);
  EXPECT_EQ(config.keep_rules.size(), 1) << rule;
  if (config.keep_rules.empty()) {
    return "";
  }
  return keep_rules::show_keep(**config.keep_rules.begin(),
                               /* show_source */ false);
}

} // namespace

// Every class specification names a kind of class. A rule that names one has
// it printed by `show_access_flags`; a plain class rule names none, and had
// nothing printed at all, so it round-tripped to "-keep Foo" - not a rule
// ProGuard accepts back.
TEST(ProguardPrintConfigurationTest, a_plain_class_rule_says_class) {
  EXPECT_EQ(round_trip("-keep class Foo"), "-keep class Foo ");
}

// The kinds that carry an access flag are printed from the flag. `interface`
// and `@interface` are different kinds of rule and have to stay apart: an '@'
// prefixed onto the interface flag turned the first into the second.
TEST(ProguardPrintConfigurationTest, class_kinds_round_trip) {
  EXPECT_EQ(round_trip("-keep enum Foo"), "-keep enum Foo ");
  EXPECT_EQ(round_trip("-keep interface Foo"), "-keep interface Foo ");
  EXPECT_EQ(round_trip("-keep @interface Foo"), "-keep @interface Foo ");
  EXPECT_EQ(round_trip("-keep !interface Foo"), "-keep !interface class Foo ");
}
