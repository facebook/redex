/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
  std::stringstream ss("{ } ( ) ; : ! , . / class public final abstract interface\n"
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
  std::vector<redex::proguard_parser::token> expected
  =   {redex::proguard_parser::token::openCurlyBracket,
       redex::proguard_parser::token::closeCurlyBracket,
			 redex::proguard_parser::token::openBracket,
			 redex::proguard_parser::token::closeBracket,
			 redex::proguard_parser::token::semiColon,
			 redex::proguard_parser::token::colon,
			 redex::proguard_parser::token::notToken,
			 redex::proguard_parser::token::comma,
			 redex::proguard_parser::token::dot,
			 redex::proguard_parser::token::slash,
			 redex::proguard_parser::token::classToken,
			 redex::proguard_parser::token::publicToken,
			 redex::proguard_parser::token::final,
			 redex::proguard_parser::token::abstract,
			 redex::proguard_parser::token::interface,
			 redex::proguard_parser::token::enumToken,
			 redex::proguard_parser::token::extends,
			 redex::proguard_parser::token::implements,
			 redex::proguard_parser::token::privateToken,
			 redex::proguard_parser::token::protectedToken,
			 redex::proguard_parser::token::staticToken,
			 redex::proguard_parser::token::volatileToken,
			 redex::proguard_parser::token::annotation_application,
			 redex::proguard_parser::token::transient,
			 redex::proguard_parser::token::annotation,
			 redex::proguard_parser::token::synchronized,
			 redex::proguard_parser::token::native,
			 redex::proguard_parser::token::strictfp,
			 redex::proguard_parser::token::synthetic,
			 redex::proguard_parser::token::bridge,
			 redex::proguard_parser::token::varargs,
			 redex::proguard_parser::token::identifier,
			 redex::proguard_parser::token::init,
			 redex::proguard_parser::token::fields,
			 redex::proguard_parser::token::methods,
			 redex::proguard_parser::token::arrayType,
			 redex::proguard_parser::token::target,
			 redex::proguard_parser::token::target_version_token,
			 redex::proguard_parser::token::include,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::basedirectory,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::injars,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::outjars,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::libraryjars,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::keepdirectories,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::keep,
			 redex::proguard_parser::token::keepclassmembernames,
			 redex::proguard_parser::token::keepnames,
			 redex::proguard_parser::token::keepnames,
			 redex::proguard_parser::token::keepclasseswithmembernames,
			 redex::proguard_parser::token::printseeds,
			 redex::proguard_parser::token::filepath,
			 redex::proguard_parser::token::includedescriptorclasses_token,
			 redex::proguard_parser::token::allowshrinking_token,
			 redex::proguard_parser::token::allowoptimization_token,
			 redex::proguard_parser::token::allowobfuscation_token,
			 redex::proguard_parser::token::dontshrink,
			 redex::proguard_parser::token::printusage,
			 redex::proguard_parser::token::whyareyoukeeping,
			 redex::proguard_parser::token::dontoptimize,
			 redex::proguard_parser::token::optimizations,
			 redex::proguard_parser::token::optimizationpasses,
			 redex::proguard_parser::token::assumenosideeffects,
			 redex::proguard_parser::token::mergeinterfacesaggressively,
			 redex::proguard_parser::token::allowaccessmodification_token,
			 redex::proguard_parser::token::printmapping,
			 redex::proguard_parser::token::repackageclasses,
			 redex::proguard_parser::token::keepattributes,
			 redex::proguard_parser::token::dontusemixedcaseclassnames_token,
			 redex::proguard_parser::token::dontpreverify_token,
			 redex::proguard_parser::token::printconfiguration,
			 redex::proguard_parser::token::dontwarn,
			 redex::proguard_parser::token::verbose_token,
			 redex::proguard_parser::token::command,
			 redex::proguard_parser::token::classToken,
			 redex::proguard_parser::token::identifier,
			 redex::proguard_parser::token::init,
			 redex::proguard_parser::token::openBracket,
			 redex::proguard_parser::token::dot,
			 redex::proguard_parser::token::dot,
			 redex::proguard_parser::token::dot,
			 redex::proguard_parser::token::closeBracket,
			 redex::proguard_parser::token::semiColon,
			 redex::proguard_parser::token::eof_token,
  };
  std::vector<std::unique_ptr<redex::proguard_parser::Token>> tokens = redex::proguard_parser::lex(ss);
  ASSERT_EQ(tokens.size(), expected.size());
  for (auto i =0; i < expected.size(); i++) {
	  if (tokens[i]->type != expected[i]) {
		  std::cout << "got " << tokens[i]->show() << " expected " << &expected[i] << std::endl ;
		}
    ASSERT_EQ(tokens[i]->type, expected[i]);
  }
}
