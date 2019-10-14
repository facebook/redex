/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <istream>
#include <vector>

#include "ProguardLexer.h"

// Make sure we can parse an empty string
TEST(ProguardLexerTest, empty) {
  std::istringstream ss("");
  std::vector<std::unique_ptr<keep_rules::proguard_parser::Token>> tokens =
      keep_rules::proguard_parser::lex(ss);
  ASSERT_EQ(tokens.size(), 1);
  ASSERT_EQ(tokens[0]->type, keep_rules::proguard_parser::token::eof_token);
}

// Parse a few tokens.
TEST(ProguardLexerTest, assortment) {
  // The ss stream below should result in the vector of tokens in the expected variable
  // that occurs below this. Please keep ss and expected in sync.
  std::stringstream ss(
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
      "<init>(...);\n");
  std::vector<std::pair<unsigned, keep_rules::proguard_parser::token>>
      expected = {
          {1, keep_rules::proguard_parser::token::openCurlyBracket},
          {1, keep_rules::proguard_parser::token::closeCurlyBracket},
          {1, keep_rules::proguard_parser::token::openBracket},
          {1, keep_rules::proguard_parser::token::closeBracket},
          {1, keep_rules::proguard_parser::token::semiColon},
          {1, keep_rules::proguard_parser::token::colon},
          {1, keep_rules::proguard_parser::token::notToken},
          {1, keep_rules::proguard_parser::token::comma},
          {1, keep_rules::proguard_parser::token::slash},
          {1, keep_rules::proguard_parser::token::classToken},
          {1, keep_rules::proguard_parser::token::publicToken},
          {1, keep_rules::proguard_parser::token::final},
          {1, keep_rules::proguard_parser::token::abstract},
          {1, keep_rules::proguard_parser::token::interface},
          {2, keep_rules::proguard_parser::token::enumToken},
          {2, keep_rules::proguard_parser::token::extends},
          {2, keep_rules::proguard_parser::token::implements},
          {2, keep_rules::proguard_parser::token::privateToken},
          {2, keep_rules::proguard_parser::token::protectedToken},
          {2, keep_rules::proguard_parser::token::staticToken},
          {3, keep_rules::proguard_parser::token::volatileToken},
          {3, keep_rules::proguard_parser::token::annotation_application},
          {3, keep_rules::proguard_parser::token::transient},
          {3, keep_rules::proguard_parser::token::annotation},
          {3, keep_rules::proguard_parser::token::synchronized},
          {3, keep_rules::proguard_parser::token::native},
          {4, keep_rules::proguard_parser::token::strictfp},
          {4, keep_rules::proguard_parser::token::synthetic},
          {4, keep_rules::proguard_parser::token::bridge},
          {4, keep_rules::proguard_parser::token::varargs},
          {4, keep_rules::proguard_parser::token::identifier},
          {4, keep_rules::proguard_parser::token::identifier},
          {4, keep_rules::proguard_parser::token::identifier},
          {5, keep_rules::proguard_parser::token::identifier},
          {5, keep_rules::proguard_parser::token::arrayType},
          {6, keep_rules::proguard_parser::token::target},
          {6, keep_rules::proguard_parser::token::target_version_token},
          {7, keep_rules::proguard_parser::token::include},
          {7, keep_rules::proguard_parser::token::filepath},
          {8, keep_rules::proguard_parser::token::basedirectory},
          {8, keep_rules::proguard_parser::token::filepath},
          {9, keep_rules::proguard_parser::token::injars},
          {9, keep_rules::proguard_parser::token::filepath},
          {10, keep_rules::proguard_parser::token::outjars},
          {10, keep_rules::proguard_parser::token::filepath},
          {10, keep_rules::proguard_parser::token::filepath},
          {11, keep_rules::proguard_parser::token::libraryjars},
          {11, keep_rules::proguard_parser::token::filepath},
          {12, keep_rules::proguard_parser::token::keepdirectories},
          {12, keep_rules::proguard_parser::token::filepath},
          {13, keep_rules::proguard_parser::token::keep},
          {13, keep_rules::proguard_parser::token::keepclassmembernames},
          {13, keep_rules::proguard_parser::token::keepnames},
          {13, keep_rules::proguard_parser::token::keepnames},
          {13, keep_rules::proguard_parser::token::keepclasseswithmembernames},
          {14, keep_rules::proguard_parser::token::printseeds},
          {14, keep_rules::proguard_parser::token::filepath},
          {15,
           keep_rules::proguard_parser::token::includedescriptorclasses_token},
          {15, keep_rules::proguard_parser::token::allowshrinking_token},
          {15, keep_rules::proguard_parser::token::allowoptimization_token},
          {15, keep_rules::proguard_parser::token::allowobfuscation_token},
          {16, keep_rules::proguard_parser::token::dontshrink},
          {16, keep_rules::proguard_parser::token::printusage},
          {16, keep_rules::proguard_parser::token::whyareyoukeeping},
          {17, keep_rules::proguard_parser::token::dontoptimize},
          {17, keep_rules::proguard_parser::token::optimizations},
          {17, keep_rules::proguard_parser::token::optimizationpasses},
          {17, keep_rules::proguard_parser::token::assumenosideeffects},
          {17, keep_rules::proguard_parser::token::mergeinterfacesaggressively},
          {17,
           keep_rules::proguard_parser::token::allowaccessmodification_token},
          {18, keep_rules::proguard_parser::token::printmapping},
          {18, keep_rules::proguard_parser::token::repackageclasses},
          {18, keep_rules::proguard_parser::token::keepattributes},
          {18, keep_rules::proguard_parser::token::
                   dontusemixedcaseclassnames_token},
          {18, keep_rules::proguard_parser::token::dontpreverify_token},
          {18, keep_rules::proguard_parser::token::printconfiguration},
          {18, keep_rules::proguard_parser::token::dontwarn},
          {19, keep_rules::proguard_parser::token::verbose_token},
          {19, keep_rules::proguard_parser::token::command},
          {20, keep_rules::proguard_parser::token::classToken},
          {20, keep_rules::proguard_parser::token::identifier},
          {21, keep_rules::proguard_parser::token::identifier},
          {21, keep_rules::proguard_parser::token::openBracket},
          {21, keep_rules::proguard_parser::token::identifier},
          {21, keep_rules::proguard_parser::token::closeBracket},
          {21, keep_rules::proguard_parser::token::semiColon},
          {22, keep_rules::proguard_parser::token::eof_token},
      };
  std::vector<std::unique_ptr<keep_rules::proguard_parser::Token>> tokens =
      keep_rules::proguard_parser::lex(ss);
  ASSERT_EQ(tokens.size(), expected.size());
  for (auto i = 0; i < expected.size(); i++) {
    std::cerr << "Performing test " << i << std::endl;
    ASSERT_EQ(expected[i].first, tokens[i]->line);
    ASSERT_EQ(expected[i].second, tokens[i]->type);
  }
}
