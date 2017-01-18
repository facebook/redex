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

#include "ProguardConfiguration.h"
#include "ProguardParser.h"

using namespace redex;

// Make sure we can parse an empty string
TEST(ProguardParserTest, empty1) {
  ProguardConfiguration config;
  std::istringstream ss1("");
  proguard_parser::parse(ss1, &config);
  ASSERT_TRUE(config.ok);
  std::istringstream ss2(" ");
  proguard_parser::parse(ss2, &config);
  ASSERT_TRUE(config.ok);
  std::istringstream ss3("  ");
  proguard_parser::parse(ss3, &config);
  ASSERT_TRUE(config.ok);
  std::istringstream ss4("\n");
  proguard_parser::parse(ss4, &config);
  ASSERT_TRUE(config.ok);
  std::istringstream ss5(" \n");
  proguard_parser::parse(ss5, &config);
  ASSERT_TRUE(config.ok);
}

// Make sure we can recognize a parsing failure.
TEST(ProguardParserTest, bad2) {
  ProguardConfiguration config;
  std::istringstream ss("~~*%^");
  proguard_parser::parse(ss, &config);
  ASSERT_FALSE(config.ok);
}

// Input/Output Options

// Parse include
TEST(ProguardParserTest, include) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-include /alpha.txt \n"
      "-include /alpha/beta.txt \n"
      "-include \"gamma.txt\" \n");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.includes.size(), 3);
  ASSERT_EQ(config.includes[0], "/alpha.txt");
  ASSERT_EQ(config.includes[1], "/alpha/beta.txt");
  ASSERT_EQ(config.includes[2], "gamma.txt");
}

// Parse basedirectory
TEST(ProguardParserTest, basedirectory) {
  ProguardConfiguration config;
  std::istringstream ss("-basedirectory /alpha/beta");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.basedirectory, "/alpha/beta");
}

// Parse keepdirectories
TEST(ProguardParserTest, keepdirectories) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-keepdirectories alpha \n"
      "-keepdirectories /alpha/beta \n"
      "-keepdirectories \"gamma\" \n"
      "-keepdirectories /alpha/beta2:gamma/delta:/iota/a/b/c/deer\n");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.keepdirectories.size(), 6);
  ASSERT_EQ(config.keepdirectories[0], "alpha");
  ASSERT_EQ(config.keepdirectories[1], "/alpha/beta");
  ASSERT_EQ(config.keepdirectories[2], "gamma");
  ASSERT_EQ(config.keepdirectories[3], "/alpha/beta2");
  ASSERT_EQ(config.keepdirectories[4], "gamma/delta");
  ASSERT_EQ(config.keepdirectories[5], "/iota/a/b/c/deer");
}

// Target
TEST(ProguardParserTest, target) {
  ProguardConfiguration config;
  std::istringstream ss("-target 1.8");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.target_version, "1.8");
}

// Misc config options.
TEST(ProguardParserTest, options1) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-dontshrink\n"
      "-allowaccessmodification -verbose\n"
      "-dontusemixedcaseclassnames\n"
      "-dontpreverify\n");
  proguard_parser::parse(ss, &config);
  ASSERT_FALSE(config.shrink);
  ASSERT_TRUE(config.allowaccessmodification);
  ASSERT_TRUE(config.dontusemixedcaseclassnames);
  ASSERT_TRUE(config.dontpreverify);
  ASSERT_TRUE(config.verbose);
}

// Parse injars
TEST(ProguardParserTest, injars) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-injars alpha.txt \n"
      "-injars alpha/beta.txt \n"
      "-injars \"gamma.txt\" \n"
      "-injars /alpha/beta2.txt:gamma/delta.txt:/iota/a/b/c/deer.txt\n");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.injars.size(), 6);
  ASSERT_EQ(config.injars[0], "alpha.txt");
  ASSERT_EQ(config.injars[1], "alpha/beta.txt");
  ASSERT_EQ(config.injars[2], "gamma.txt");
  ASSERT_EQ(config.injars[3], "/alpha/beta2.txt");
  ASSERT_EQ(config.injars[4], "gamma/delta.txt");
  ASSERT_EQ(config.injars[5], "/iota/a/b/c/deer.txt");
}

// Parse outjars
TEST(ProguardParserTest, outjars) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-outjars alpha.txt \n"
      "-outjars alpha/beta.txt \n"
      "-outjars \"gamma.txt\" \n"
      "-outjars /alpha/beta2.txt:gamma/delta.txt:/iota/a/b/c/deer.txt\n");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.outjars.size(), 6);
  ASSERT_EQ(config.outjars[0], "alpha.txt");
  ASSERT_EQ(config.outjars[1], "alpha/beta.txt");
  ASSERT_EQ(config.outjars[2], "gamma.txt");
  ASSERT_EQ(config.outjars[3], "/alpha/beta2.txt");
  ASSERT_EQ(config.outjars[4], "gamma/delta.txt");
  ASSERT_EQ(config.outjars[5], "/iota/a/b/c/deer.txt");
}

// Parse libraryjars
TEST(ProguardParserTest, libraryjars) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-libraryjars alpha.txt \n"
      "-libraryjars alpha/beta.txt \n"
      "-libraryjars \"gamma.txt\" \n"
      "-libraryjars /alpha/beta2.txt:gamma/delta.txt:/iota/a/b/c/deer.txt\n");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.libraryjars.size(), 6);
  ASSERT_EQ(config.libraryjars[0], "alpha.txt");
  ASSERT_EQ(config.libraryjars[1], "alpha/beta.txt");
  ASSERT_EQ(config.libraryjars[2], "gamma.txt");
  ASSERT_EQ(config.libraryjars[3], "/alpha/beta2.txt");
  ASSERT_EQ(config.libraryjars[4], "gamma/delta.txt");
  ASSERT_EQ(config.libraryjars[5], "/iota/a/b/c/deer.txt");
}

// Keep Options

// keep
TEST(ProguardParserTest, keep) {
  ProguardConfiguration config1;
  std::istringstream ss1("-keep class Alpha");
  proguard_parser::parse(ss1, &config1);
  ASSERT_EQ(config1.keep_rules.size(), 1);
  KeepSpec k = config1.keep_rules[0];
  ClassSpecification cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha");
  ASSERT_EQ(cs.setAccessFlags, 0);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config2;
  std::istringstream ss2("-keep class Alpha.Beta");
  proguard_parser::parse(ss2, &config2);
  ASSERT_EQ(config2.keep_rules.size(), 1);
  k = config2.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, 0);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config3;
  std::istringstream ss3(
      "-keep @com.facebook.crypto.proguard.annotations.DoNotStrip class "
      "Alpha.Beta");
  proguard_parser::parse(ss3, &config3);
  ASSERT_EQ(config3.keep_rules.size(), 1);
  k = config3.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, 0);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType,
            "Lcom/facebook/crypto/proguard/annotations/DoNotStrip;");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config4;
  std::istringstream ss4("-keep enum Alpha.Beta");
  proguard_parser::parse(ss4, &config4);
  ASSERT_EQ(config4.keep_rules.size(), 1);
  k = config4.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, ACC_ENUM);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config5;
  std::istringstream ss5("-keep interface Alpha.Beta");
  proguard_parser::parse(ss5, &config5);
  ASSERT_EQ(config5.keep_rules.size(), 1);
  k = config5.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, ACC_INTERFACE);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config6;
  std::istringstream ss6("-keep public class Alpha.Beta");
  proguard_parser::parse(ss6, &config6);
  ASSERT_EQ(config6.keep_rules.size(), 1);
  k = config6.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, ACC_PUBLIC);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config7;
  std::istringstream ss7("-keep !public class Alpha.Beta");
  proguard_parser::parse(ss7, &config7);
  ASSERT_EQ(config7.keep_rules.size(), 1);
  k = config7.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, 0);
  ASSERT_EQ(cs.unsetAccessFlags, ACC_PUBLIC);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config8;
  std::istringstream ss8("-keep !public final class Alpha.Beta");
  proguard_parser::parse(ss8, &config8);
  ASSERT_EQ(config8.keep_rules.size(), 1);
  k = config8.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, ACC_FINAL);
  ASSERT_EQ(cs.unsetAccessFlags, ACC_PUBLIC);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);

  ProguardConfiguration config9;
  std::istringstream ss9("-keep abstract class Alpha.Beta");
  proguard_parser::parse(ss9, &config9);
  ASSERT_EQ(config9.keep_rules.size(), 1);
  k = config9.keep_rules[0];
  cs = k.class_spec;
  ASSERT_EQ(cs.className, "Alpha.Beta");
  ASSERT_EQ(cs.setAccessFlags, ACC_ABSTRACT);
  ASSERT_EQ(cs.unsetAccessFlags, 0);
  ASSERT_EQ(cs.extendsAnnotationType, "");
  ASSERT_EQ(cs.extendsClassName, "");
  ASSERT_EQ(cs.annotationType, "");
  ASSERT_EQ(cs.fieldSpecifications.size(), 0);
  ASSERT_EQ(cs.methodSpecifications.size(), 0);
}

// Shrinking Options

// dontshrink
TEST(ProguardParserTest, dontshrink) {
  ProguardConfiguration config;
  std::istringstream ss("-dontshrink");
  proguard_parser::parse(ss, &config);
  ASSERT_FALSE(config.shrink);
}

// printusage
TEST(ProguardParserTest, printusage) {
  ProguardConfiguration config;
  std::istringstream ss("-printusage /alpha/beta.txt");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.printusage.size(), 1);
  ASSERT_EQ(config.printusage[0], "/alpha/beta.txt");
}

// whyareyoukeeping
TEST(ProguardParserTest, whyareyoukeeping) {
  ProguardConfiguration config;
  std::istringstream ss("-whyareyoukeeping class Alpha.Beta");
  proguard_parser::parse(ss, &config);
  ASSERT_TRUE(config.ok);
}

// Optimization Options

// dontoptimize
TEST(ProguardParserTest, dontoptimize) {
  ProguardConfiguration config;
  ASSERT_TRUE(config.optimize);
  std::istringstream ss("-dontoptimize");
  proguard_parser::parse(ss, &config);
  ASSERT_FALSE(config.optimize);
}

// optimizations
TEST(ProguardParserTest, optimizations) {
  ProguardConfiguration config;
  std::istringstream ss(
      "-optimizations "
      "!code/simplification/arithmetic,!code/simplification/cast,!field/"
      "*,!class/merging/*,!field/propagation/value, !method/propagation/"
      "parameter,!method/propagation/returnvalue,!code/simplification/"
      "arithmetic");
  proguard_parser::parse(ss, &config);
  ASSERT_EQ(config.optimization_filters.size(), 8);
  ASSERT_EQ(config.optimization_filters[0], "!code/simplification/arithmetic");
  ASSERT_EQ(config.optimization_filters[1], "!code/simplification/cast");
  ASSERT_EQ(config.optimization_filters[2], "!field/*");
  ASSERT_EQ(config.optimization_filters[3], "!class/merging/*");
  ASSERT_EQ(config.optimization_filters[4], "!field/propagation/value");
  ASSERT_EQ(config.optimization_filters[5], "!method/propagation/parameter");
  ASSERT_EQ(config.optimization_filters[6], "!method/propagation/returnvalue");
  ASSERT_EQ(config.optimization_filters[7], "!code/simplification/arithmetic");
}

// Handling of @interface
TEST(ProguardParserTest, annotationclass) {
  ProguardConfiguration config;
  ASSERT_TRUE(config.optimize);
  std::istringstream ss("-keep @interface *");
  proguard_parser::parse(ss, &config);
  ASSERT_TRUE(config.ok);
  ASSERT_EQ(config.keep_rules.size(), 1);
  ASSERT_EQ(config.keep_rules[0].class_spec.className, "*");
  ASSERT_EQ(config.keep_rules[0].class_spec.setAccessFlags, ACC_ANNOTATION);
}

// Member specifications
TEST(ProguardParserTest, member_specification) {
  {
    ProguardConfiguration config;
    std::istringstream ss("-keep class Alpha { *; }");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss("-keep class Alpha { <methods>; }");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss("-keep class Alpha { <fields>; }");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 0);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keepclasseswithmembers,allowshrinking class * {"
        "  native <methods>;"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
  }
}

// Method member specifications
TEST(ProguardParserTest, method_member_specification) {
  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public int omega(int, boolean, java.lang.String, char);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(IZLjava/lang/String;C)I", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega();"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("()V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(int);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(I)V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(java.lang.String);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(Ljava/lang/String;)V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(%);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(%)V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(java.lang.Str?ng);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(Ljava/lang/Str?ng;)V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(java.*.String);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(Ljava/*/String;)V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(java.**.String);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(Ljava/**/String;)V", keep.descriptor);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(***);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(***)V", keep.descriptor);
    ASSERT_FALSE(config.keep_rules[0].allowshrinking);
  }

  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep class * {"
        "  public void omega(...);"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_EQ(config.keep_rules[0].class_spec.fieldSpecifications.size(), 0);
    ASSERT_EQ(config.keep_rules[0].class_spec.methodSpecifications.size(), 1);
    auto keep = config.keep_rules[0].class_spec.methodSpecifications[0];
    ASSERT_EQ("(...)V", keep.descriptor);
  }
}

TEST(ProguardParserTest, keepnames) {
  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keepnames class * {"
        "  int wombat();"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_TRUE(config.keep_rules[0].allowshrinking);
  }
}


TEST(ProguardParserTest, keepclassmembernames) {
  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keepclassmembernames class * {"
        "  int wombat();"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_TRUE(config.keep_rules[0].allowshrinking);
  }
}

TEST(ProguardParserTest, keepclasseswithmembernames) {
  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keepclasseswithmembernames class * {"
        "  int wombat();"
        "}");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_TRUE(config.keep_rules[0].allowshrinking);
  }
}

TEST(ProguardParserTest, keep_annotation_classes) {
  {
    ProguardConfiguration config;
    std::istringstream ss(
        "-keep @interface *");
    proguard_parser::parse(ss, &config);
    ASSERT_TRUE(config.ok);
    ASSERT_EQ(config.keep_rules.size(), 1);
    ASSERT_FALSE(config.keep_rules[0].allowshrinking);
    ASSERT_EQ(config.keep_rules[0].class_spec.setAccessFlags, ACC_ANNOTATION);
  }
}
