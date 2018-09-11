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
  std::vector<std::unique_ptr<redex::proguard_parser::Token>> tokens = redex::proguard_parser::lex(ss);
  ASSERT_EQ(tokens.size(), 1);
  ASSERT_EQ(tokens[0]->type, redex::proguard_parser::token::eof_token);
}

// Parse a few tokens.
TEST(ProguardLexerTest, assortment) {
  // The ss stream below should result in the vector of tokens in the expected variable
  // that occurs below this. Please keep ss and expected in sync.
  std::stringstream ss("{ } ( ) ; : ! , / class public final abstract interface\n"
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
                       "-keep -keepclassmembernames -keepnames -keepnames -keepclasseswithmembernames\n"
                       "-printseeds seedsfile.txt\n"
                       "includedescriptorclasses allowshrinking allowoptimization allowobfuscation\n"
                       "-dontshrink -printusage -whyareyoukeeping\n"
                       "-dontoptimize -optimizations -optimizationpasses -assumenosideeffects -mergeinterfacesaggressively -allowaccessmodification\n"
                       "-printmapping -repackageclasses -keepattributes -dontusemixedcaseclassnames -dontpreverify -printconfiguration -dontwarn\n"
                       "-verbose -someothercommand\n"
                       "class com.google.android.gms.measurement.AppMeasurementService\n"
                       "<init>(...);\n"
                       );
  std::vector<std::pair<unsigned, redex::proguard_parser::token>> expected
    =   {{1, redex::proguard_parser::token::openCurlyBracket},
         {1, redex::proguard_parser::token::closeCurlyBracket},
         {1, redex::proguard_parser::token::openBracket},
         {1, redex::proguard_parser::token::closeBracket},
         {1, redex::proguard_parser::token::semiColon},
         {1, redex::proguard_parser::token::colon},
         {1, redex::proguard_parser::token::notToken},
         {1, redex::proguard_parser::token::comma},
         {1, redex::proguard_parser::token::slash},
         {1, redex::proguard_parser::token::classToken},
         {1, redex::proguard_parser::token::publicToken},
         {1, redex::proguard_parser::token::final},
         {1, redex::proguard_parser::token::abstract},
         {1, redex::proguard_parser::token::interface},
         {2, redex::proguard_parser::token::enumToken},
         {2, redex::proguard_parser::token::extends},
         {2, redex::proguard_parser::token::implements},
         {2, redex::proguard_parser::token::privateToken},
         {2, redex::proguard_parser::token::protectedToken},
         {2, redex::proguard_parser::token::staticToken},
         {3, redex::proguard_parser::token::volatileToken},
         {3, redex::proguard_parser::token::annotation_application},
         {3, redex::proguard_parser::token::transient},
         {3, redex::proguard_parser::token::annotation},
         {3, redex::proguard_parser::token::synchronized},
         {3, redex::proguard_parser::token::native},
         {4, redex::proguard_parser::token::strictfp},
         {4, redex::proguard_parser::token::synthetic},
         {4, redex::proguard_parser::token::bridge},
         {4, redex::proguard_parser::token::varargs},
         {4, redex::proguard_parser::token::identifier},
         {4, redex::proguard_parser::token::identifier},
         {4, redex::proguard_parser::token::identifier},
         {5, redex::proguard_parser::token::identifier},
         {5, redex::proguard_parser::token::arrayType},
         {6, redex::proguard_parser::token::target},
         {6, redex::proguard_parser::token::target_version_token},
         {7, redex::proguard_parser::token::include},
         {7, redex::proguard_parser::token::filepath},
         {8, redex::proguard_parser::token::basedirectory},
         {8, redex::proguard_parser::token::filepath},
         {9, redex::proguard_parser::token::injars},
         {9, redex::proguard_parser::token::filepath},
         {10, redex::proguard_parser::token::outjars},
         {10, redex::proguard_parser::token::filepath},
         {10, redex::proguard_parser::token::filepath},
         {11, redex::proguard_parser::token::libraryjars},
         {11, redex::proguard_parser::token::filepath},
         {12, redex::proguard_parser::token::keepdirectories},
         {12, redex::proguard_parser::token::filepath},
         {13, redex::proguard_parser::token::keep},
         {13, redex::proguard_parser::token::keepclassmembernames},
         {13, redex::proguard_parser::token::keepnames},
         {13, redex::proguard_parser::token::keepnames},
         {13, redex::proguard_parser::token::keepclasseswithmembernames},
         {14, redex::proguard_parser::token::printseeds},
         {14, redex::proguard_parser::token::filepath},
         {15, redex::proguard_parser::token::includedescriptorclasses_token},
         {15, redex::proguard_parser::token::allowshrinking_token},
         {15, redex::proguard_parser::token::allowoptimization_token},
         {15, redex::proguard_parser::token::allowobfuscation_token},
         {16, redex::proguard_parser::token::dontshrink},
         {16, redex::proguard_parser::token::printusage},
         {16, redex::proguard_parser::token::whyareyoukeeping},
         {17, redex::proguard_parser::token::dontoptimize},
         {17, redex::proguard_parser::token::optimizations},
         {17, redex::proguard_parser::token::optimizationpasses},
         {17, redex::proguard_parser::token::assumenosideeffects},
         {17, redex::proguard_parser::token::mergeinterfacesaggressively},
         {17, redex::proguard_parser::token::allowaccessmodification_token},
         {18, redex::proguard_parser::token::printmapping},
         {18, redex::proguard_parser::token::repackageclasses},
         {18, redex::proguard_parser::token::keepattributes},
         {18, redex::proguard_parser::token::dontusemixedcaseclassnames_token},
         {18, redex::proguard_parser::token::dontpreverify_token},
         {18, redex::proguard_parser::token::printconfiguration},
         {18, redex::proguard_parser::token::dontwarn},
         {19, redex::proguard_parser::token::verbose_token},
         {19, redex::proguard_parser::token::command},
         {20, redex::proguard_parser::token::classToken},
         {20, redex::proguard_parser::token::identifier},
         {21, redex::proguard_parser::token::identifier},
         {21, redex::proguard_parser::token::openBracket},
         {21, redex::proguard_parser::token::identifier},
         {21, redex::proguard_parser::token::closeBracket},
         {21, redex::proguard_parser::token::semiColon},
         {22, redex::proguard_parser::token::eof_token},
  };
  std::vector<std::unique_ptr<redex::proguard_parser::Token>> tokens = redex::proguard_parser::lex(ss);
  ASSERT_EQ(tokens.size(), expected.size());
  for (auto i = 0; i < expected.size(); i++) {
    std::cerr << "Performing test " << i << std::endl;
    ASSERT_EQ(expected[i].first, tokens[i]->line);
    ASSERT_EQ(expected[i].second, tokens[i]->type);
  }
}
