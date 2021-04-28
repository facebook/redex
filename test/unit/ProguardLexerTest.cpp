/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <istream>
#include <vector>

#include "ProguardLexer.h"

using namespace keep_rules::proguard_parser;

// Make sure we can parse an empty string
TEST(ProguardLexerTest, empty) {
  std::vector<Token> tokens = lex("");
  ASSERT_EQ(tokens.size(), 1);
  ASSERT_EQ(tokens[0].type, TokenType::eof_token);
}

// Parse a few tokens.
TEST(ProguardLexerTest, assortment) {
  // The ss stream below should result in the vector of tokens in the expected
  // variable that occurs below this. Please keep ss and expected in sync.
  const char* s =
      "{ } ( ) ; : ! , / class public final abstract interface\n"
      "enum extends implements private protected static\n"
      "volatile @ transient @interface synchronized native\n"
      "strictfp synthetic bridge varargs wombat <init> <fields>\n"
      "<methods> []\n"
      "-target 1.8 \n"
      "-include /alpha/beta.pro\n"
      "-basedirectory /alpha/beta\n"
      "-injars gamma.pro\n"
      "-outjars delta.pro:/epsilon/iota.pro\n"
      "-libraryjars /alpha/zeta.pro\n"
      "-keepdirectories mydir/**\n"
      "-keep -keepclassmembernames -keepnames -keepnames "
      "-keepclasseswithmembernames\n"
      "-printseeds seedsfile.txt\n"
      "includedescriptorclasses allowshrinking allowoptimization "
      "allowobfuscation\n"
      "-dontshrink -printusage -whyareyoukeeping\n"
      "-dontoptimize -optimizations -optimizationpasses -assumenosideeffects "
      "-mergeinterfacesaggressively -allowaccessmodification\n"
      "-printmapping -repackageclasses -keepattributes "
      "-dontusemixedcaseclassnames -dontpreverify -printconfiguration "
      "-dontwarn\n"
      "-verbose -someothercommand\n"
      "class com.google.android.gms.measurement.AppMeasurementService\n"
      "<init>(...);\n"
      "-keep class *#-keepnames class *\n"
      "-dontobfuscate\n";
  std::vector<std::pair<unsigned, TokenType>> expected = {
      {1, TokenType::openCurlyBracket},
      {1, TokenType::closeCurlyBracket},
      {1, TokenType::openBracket},
      {1, TokenType::closeBracket},
      {1, TokenType::semiColon},
      {1, TokenType::colon},
      {1, TokenType::notToken},
      {1, TokenType::comma},
      {1, TokenType::slash},
      {1, TokenType::classToken},
      {1, TokenType::publicToken},
      {1, TokenType::final},
      {1, TokenType::abstract},
      {1, TokenType::interface},
      {2, TokenType::enumToken},
      {2, TokenType::extends},
      {2, TokenType::implements},
      {2, TokenType::privateToken},
      {2, TokenType::protectedToken},
      {2, TokenType::staticToken},
      {3, TokenType::volatileToken},
      {3, TokenType::annotation_application},
      {3, TokenType::transient},
      {3, TokenType::annotation},
      {3, TokenType::synchronized},
      {3, TokenType::native},
      {4, TokenType::strictfp},
      {4, TokenType::synthetic},
      {4, TokenType::bridge},
      {4, TokenType::varargs},
      {4, TokenType::identifier},
      {4, TokenType::identifier},
      {4, TokenType::identifier},
      {5, TokenType::identifier},
      {5, TokenType::arrayType},
      {6, TokenType::target},
      {6, TokenType::target_version_token},
      {7, TokenType::include},
      {7, TokenType::filepath},
      {8, TokenType::basedirectory},
      {8, TokenType::filepath},
      {9, TokenType::injars},
      {9, TokenType::filepath},
      {10, TokenType::outjars},
      {10, TokenType::filepath},
      {10, TokenType::filepath},
      {11, TokenType::libraryjars},
      {11, TokenType::filepath},
      {12, TokenType::keepdirectories},
      {12, TokenType::filepath},
      {13, TokenType::keep},
      {13, TokenType::keepclassmembernames},
      {13, TokenType::keepnames},
      {13, TokenType::keepnames},
      {13, TokenType::keepclasseswithmembernames},
      {14, TokenType::printseeds},
      {14, TokenType::filepath},
      {15, TokenType::includedescriptorclasses_token},
      {15, TokenType::allowshrinking_token},
      {15, TokenType::allowoptimization_token},
      {15, TokenType::allowobfuscation_token},
      {16, TokenType::dontshrink},
      {16, TokenType::printusage},
      {16, TokenType::whyareyoukeeping},
      {17, TokenType::dontoptimize},
      {17, TokenType::optimizations},
      {17, TokenType::optimizationpasses},
      {17, TokenType::assumenosideeffects},
      {17, TokenType::mergeinterfacesaggressively},
      {17, TokenType::allowaccessmodification_token},
      {18, TokenType::printmapping},
      {18, TokenType::repackageclasses},
      {18, TokenType::keepattributes},
      {18, TokenType::dontusemixedcaseclassnames_token},
      {18, TokenType::dontpreverify_token},
      {18, TokenType::printconfiguration},
      {18, TokenType::dontwarn},
      {19, TokenType::verbose_token},
      {19, TokenType::command},
      {20, TokenType::classToken},
      {20, TokenType::identifier},
      {21, TokenType::identifier},
      {21, TokenType::openBracket},
      {21, TokenType::identifier},
      {21, TokenType::closeBracket},
      {21, TokenType::semiColon},
      {22, TokenType::keep},
      {22, TokenType::classToken},
      {22, TokenType::identifier},
      {23, TokenType::dontobfuscate},
      {24, TokenType::eof_token},
  };
  std::vector<Token> tokens = lex(s);
  EXPECT_EQ(tokens.size(), expected.size());
  for (auto i = 0; i < expected.size(); i++) {
    std::cerr << "Performing test " << i << std::endl;
    EXPECT_EQ(expected[i].first, tokens[i].line);
    EXPECT_EQ(expected[i].second, tokens[i].type) << tokens[i].show();
  }
  for (auto i = expected.size(); i < tokens.size(); ++i) {
    EXPECT_TRUE(false) << "Unexpected token " << tokens[i].show();
  }
}
