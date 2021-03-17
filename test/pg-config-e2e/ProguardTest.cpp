/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "JarLoader.h"
#include "Match.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "ProguardPrintConfiguration.h" // New ProGuard configuration
#include "ReachableClasses.h"
#include "RedexTest.h"
#include "Show.h"
#include "Trace.h"

using namespace keep_rules;

/**
The objective of these tests are to make sure the ProGuard rules are
properly applied to a set of test classes. The incomming APK is currently
already processed by ProGuard. This test makes sure the expected classes
and methods are present (or absent) as required and performs checks on the
Redex ProGuard rule matcher to make sure the ProGuard rules were properly
interpreted.
**/

DexClass* find_class_named(const DexClasses& classes, const std::string& name) {
  auto it =
      std::find_if(classes.begin(), classes.end(), [&name](DexClass* cls) {
        return name == cls->get_deobfuscated_name();
      });
  if (it == classes.end()) {
    return nullptr;
  } else {
    return *it;
  }
}

template <class Container>
DexMethod* find_method_named(const Container& methods,
                             const std::string& name) {
  TRACE(PGR, 8, "==> Searching for method %s", name.c_str());
  auto it = std::find_if(methods.begin(), methods.end(), [&name](DexMethod* m) {
    const auto& deobfuscated_method = m->get_deobfuscated_name();
    TRACE(PGR,
          8,
          "====> Comparing against method %s [%s]",
          m->c_str(),
          deobfuscated_method.c_str());
    bool found =
        (name == std::string(m->c_str()) || (name == deobfuscated_method));
    if (found) {
      TRACE(PGR, 8, "=====> Found %s.", name.c_str());
    }
    return found;
  });
  if (it == methods.end()) {
    TRACE(PGR, 8, "===> %s not found.", name.c_str());
  } else {
    TRACE(PGR, 8, "===> %s found.", name.c_str());
  }
  return it == methods.end() ? nullptr : *it;
}

DexMethod* find_vmethod_named(const DexClass* cls, const std::string& name) {
  return find_method_named(cls->get_vmethods(), name);
}

DexMethod* find_dmethod_named(const DexClass* cls, const std::string& name) {
  return find_method_named(cls->get_dmethods(), name);
}

template <class Container>
DexField* find_field_named(const Container& fields, const char* name) {
  TRACE(PGR, 8, "==> Searching for field %s", name);
  auto it = std::find_if(fields.begin(), fields.end(), [&name](DexField* f) {
    auto deobfuscated_field = f->get_deobfuscated_name();
    TRACE(PGR,
          8,
          "====> Comparing against %s [%s] <%s>",
          f->c_str(),
          SHOW(f),
          deobfuscated_field.c_str());
    bool found =
        (name == std::string(f->c_str()) || (name == deobfuscated_field));
    if (found) {
      TRACE(PGR, 8, "====> Matched.");
    }
    return found;
  });
  return it == fields.end() ? nullptr : *it;
}

DexField* find_instance_field_named(const DexClass* cls, const char* name) {
  return find_field_named(cls->get_ifields(), name);
}

DexField* find_static_field_named(const DexClass* cls, const char* name) {
  return find_field_named(cls->get_sfields(), name);
}

class ProguardTest : public RedexTest {};
/**
 * Ensure the ProGuard test rules are properly applied.
 */
TEST_F(ProguardTest, assortment) {
  const char* dexfile = std::getenv("pg_config_e2e_dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();

  // Load the Proguard map
  const char* mapping_file = std::getenv("pg_config_e2e_mapping");
  ASSERT_NE(nullptr, mapping_file);
  const ProguardMap proguard_map((std::string(mapping_file)));

  const char* configuraiton_file = std::getenv("pg_config_e2e_pgconfig");
  ASSERT_NE(nullptr, configuraiton_file);
  keep_rules::ProguardConfiguration pg_config;
  keep_rules::proguard_parser::parse_file(configuraiton_file, &pg_config);
  EXPECT_TRUE(pg_config.ok);

  auto android_env_sdk = std::getenv("ANDROID_SDK");
  auto android_config_sdk = std::getenv("sdk_path");

  auto android_sdk = (strncmp(android_config_sdk, "None", 4) != 0)
                         ? android_config_sdk
                         : android_env_sdk;

  ASSERT_NE(nullptr, android_sdk);
  auto android_target = std::getenv("android_target");
  ASSERT_NE(nullptr, android_target);
  std::string android_version(android_target);
  ASSERT_NE("NotFound", android_version);
  std::string sdk_jar = std::string(android_sdk) + "/platforms/" +
                        android_version + "/android.jar";
  Scope external_classes;
  EXPECT_TRUE(load_jar_file(sdk_jar.c_str(), &external_classes));

  Scope scope = build_class_scope(dexen);
  apply_deobfuscated_names(dexen, proguard_map);
  ConcurrentSet<const keep_rules::KeepSpec*> unused_rules =
      process_proguard_rules(proguard_map, scope, external_classes, pg_config,
                             true);

  // Check the top level Android activity class
  {
    auto proguard_test = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/ProguardTest;");
    ASSERT_NE(nullptr, proguard_test);
    EXPECT_TRUE(root(proguard_test));
    auto init = find_dmethod_named(
        proguard_test,
        "Lcom/facebook/redex/test/proguard/ProguardTest;.<init>:()V");
    ASSERT_NE(nullptr, init);
    EXPECT_TRUE(root(init));
  }

  { // Alpha is explicitly used and should not be deleted.
    auto alpha =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Alpha;");
    ASSERT_NE(alpha, nullptr);
    EXPECT_FALSE(root(alpha));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(alpha));
  }

  { // Beta is not used so should not have a keep marker.
    auto beta =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Beta;");
    ASSERT_NE(nullptr, beta);
    EXPECT_FALSE(root(beta));
  }

  { // Gamma is not used anywhere but the class only is kept by the config.
    auto gamma =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Gamma;");
    ASSERT_NE(gamma, nullptr);
    EXPECT_TRUE(root(gamma));
    auto wombat = find_instance_field_named(
        gamma, "Lcom/facebook/redex/test/proguard/Gamma;.wombat:I");
    ASSERT_NE(nullptr, wombat);
    EXPECT_FALSE(root(wombat)); // Will be kept by reachability analysis.
  }

  { // Make sure !public static <fields> is observed and check
    // handling of <init> constructors.
    auto delta =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta;");
    ASSERT_NE(nullptr, delta);
    EXPECT_TRUE(root(delta));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(delta));
    // The field "public static int alpha" should not match because of the
    // public.
    auto alpha = find_static_field_named(
        delta, "Lcom/facebook/redex/test/proguard/Delta;.alpha:I");
    ASSERT_NE(nullptr, alpha);
    EXPECT_FALSE(root(alpha));
    // The field "private static int beta" should match because it is
    // private (i.e. not public) and static.
    auto beta = find_static_field_named(
        delta, "Lcom/facebook/redex/test/proguard/Delta;.beta:I");
    ASSERT_NE(nullptr, beta);
    EXPECT_TRUE(root(beta));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(beta));
    // The field "private int gamma" should not match because
    // it is an instance field.
    auto gamma = find_instance_field_named(
        delta, "Lcom/facebook/redex/test/proguard/Delta;.gamma:I");
    ASSERT_NE(nullptr, gamma);
    EXPECT_FALSE(root(gamma));
    // Check constructors.
    auto init_V = find_dmethod_named(
        delta, "Lcom/facebook/redex/test/proguard/Delta;.<init>:()V");
    ASSERT_NE(nullptr, init_V);
    EXPECT_TRUE(root(init_V));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(init_V));
    auto init_I = find_dmethod_named(
        delta, "Lcom/facebook/redex/test/proguard/Delta;.<init>:(I)V");
    ASSERT_NE(nullptr, init_I);
    EXPECT_FALSE(root(init_I));
    auto init_S = find_dmethod_named(delta,
                                     "Lcom/facebook/redex/test/proguard/"
                                     "Delta;.<init>:(Ljava/lang/String;)V");
    ASSERT_NE(nullptr, init_S);
    EXPECT_TRUE(root(init_S));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(init_S));
    // Check clinit
    auto clinit = find_dmethod_named(
        delta, "Lcom/facebook/redex/test/proguard/Delta;.<clinit>:()V");
    ASSERT_NE(nullptr, clinit);
    EXPECT_FALSE(impl::KeepState::allowobfuscation(clinit));
  }

  { // Inner class Delta.A should be marked for keep.
    auto delta_a =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$A;");
    ASSERT_NE(nullptr, delta_a);
    EXPECT_FALSE(root(delta_a));
  }

  { // Inner class Delta.B is preserved by a keep directive.
    auto delta_b =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$B;");
    ASSERT_NE(delta_b, nullptr);
    EXPECT_TRUE(root(delta_b));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(delta_b));
  }

  { // Inner class Delta.C is kept.
    auto delta_c =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$C;");
    ASSERT_NE(delta_c, nullptr);
    EXPECT_TRUE(root(delta_c));
    // Make sure its fields and methods have been kept by the "*;" directive.
    auto iField = find_instance_field_named(
        delta_c, "Lcom/facebook/redex/test/proguard/Delta$C;.i:I");
    ASSERT_NE(iField, nullptr);
    EXPECT_TRUE(root(iField));
    auto iValue = find_vmethod_named(
        delta_c, "Lcom/facebook/redex/test/proguard/Delta$C;.iValue:()I");
    ASSERT_NE(iValue, nullptr);
  }

  { // Inner class Delta.D is kept.
    auto delta_d =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$D;");
    ASSERT_NE(delta_d, nullptr);
    EXPECT_TRUE(root(delta_d));
    // Make sure its fields are kept by "<fields>" but not its methods.
    auto iField = find_instance_field_named(
        delta_d, "Lcom/facebook/redex/test/proguard/Delta$D;.i:I");
    ASSERT_NE(nullptr, iField);
    auto iValue = find_vmethod_named(
        delta_d, "Lcom/facebook/redex/test/proguard/Delta$D;.iValue()I");
    ASSERT_EQ(nullptr, iValue);
  }

  { // Inner class Delta.E is kept and methods are kept but not fields.
    auto delta_e =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$E;");
    ASSERT_NE(delta_e, nullptr);
    EXPECT_TRUE(root(delta_e));
    // Make sure its methods are kept by "<methods>" but not its fields.
    auto iField = find_instance_field_named(
        delta_e, "Lcom/facebook/redex/test/proguard/Delta$E;.i:I");
    ASSERT_NE(nullptr, iField);
    EXPECT_FALSE(root(iField));
    auto iValue = find_vmethod_named(
        delta_e, "Lcom/facebook/redex/test/proguard/Delta$E;.iValue:()I");
    ASSERT_NE(nullptr, iValue);
  }

  { // Inner class Delta.F is kept and its final fields are kept.
    auto delta_f =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$F;");
    ASSERT_NE(delta_f, nullptr);
    EXPECT_TRUE(root(delta_f));
    // Make sure only the final fields are kept.
    // wombat is not a final field, so it should not be kept.
    auto wombatField = find_instance_field_named(
        delta_f, "Lcom/facebook/redex/test/proguard/Delta$F;.wombat:I");
    ASSERT_NE(nullptr, wombatField);
    EXPECT_FALSE(root(wombatField));
    // numbat is a final field so it should be kept
    auto numbatField = find_instance_field_named(
        delta_f, "Lcom/facebook/redex/test/proguard/Delta$F;.numbat:I");
    ASSERT_NE(numbatField, nullptr);
    EXPECT_TRUE(root(numbatField));
    // The numbatValue method should not be kept.
    auto numbatValue = find_vmethod_named(
        delta_f, "Lcom/facebook/redex/test/proguard/Delta$F;.numbatValue:()I");
    ASSERT_NE(nullptr, numbatValue);
    EXPECT_FALSE(root(numbatValue));
  }

  { // Inner class Delta.G is kept, make sure constructor is not renamed.
    auto delta_g =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$G;");
    ASSERT_NE(nullptr, delta_g);
    EXPECT_TRUE(root(delta_g));
    EXPECT_TRUE(impl::KeepState::allowobfuscation(delta_g));
    // Make sure its fields and methods have been kept by the "*;" directive.
    auto fuzzyWombat = find_instance_field_named(
        delta_g, "Lcom/facebook/redex/test/proguard/Delta$G;.fuzzyWombat:I");
    ASSERT_NE(nullptr, fuzzyWombat);
    auto fuzzyWombatValue = find_vmethod_named(
        delta_g,
        "Lcom/facebook/redex/test/proguard/Delta$G;.fuzzyWombatValue:()I");
    ASSERT_NE(nullptr, fuzzyWombatValue);
    EXPECT_TRUE(root(fuzzyWombatValue));
    EXPECT_TRUE(impl::KeepState::allowobfuscation(fuzzyWombatValue));
    // Check that the constructor is not renamed.
    auto init_V =
        find_dmethod_named(delta_g,
                           "Lcom/facebook/redex/test/proguard/"
                           "Delta$G;.<init>:(Lcom/facebook/redex/test/"
                           "proguard/Delta;)V");
    ASSERT_NE(nullptr, init_V);
    EXPECT_FALSE(impl::KeepState::allowobfuscation(init_V));
  }

  { // Inner class Delta.H is kept.
    // The config only keeps the int wombat field, everything else
    // should be removed.
    auto delta_h =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$H;");
    ASSERT_NE(delta_h, nullptr);
    EXPECT_TRUE(root(delta_h));
    EXPECT_TRUE(impl::KeepState::allowobfuscation(delta_h));
    auto wombatField = find_instance_field_named(
        delta_h, "Lcom/facebook/redex/test/proguard/Delta$H;.wombat:I");
    ASSERT_NE(wombatField, nullptr);
    EXPECT_TRUE(root(wombatField));
    auto numbatField = find_instance_field_named(
        delta_h, "Lcom/facebook/redex/test/proguard/Delta$H;.numbat:Z");
    ASSERT_NE(nullptr, numbatField);
    EXPECT_FALSE(root(numbatField));
    auto myIntValue = find_vmethod_named(
        delta_h, "Lcom/facebook/redex/test/proguard/Delta$H;.myIntValue:()I");
    ASSERT_NE(nullptr, myIntValue);
    EXPECT_FALSE(root(myIntValue));
    auto myBoolValue = find_vmethod_named(
        delta_h, "Lcom/facebook/redex/test/proguard/Delta$H;.myBoolValue:()Z");
    ASSERT_NE(nullptr, myBoolValue);
    EXPECT_FALSE(root(myBoolValue));
  }

  { // Tests for field * regex matching.
    auto delta_i =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$I;");
    ASSERT_NE(delta_i, nullptr);
    EXPECT_TRUE(root(delta_i));
    EXPECT_TRUE(impl::KeepState::allowobfuscation(delta_i));
    // Make sure all the wombat* fields were found.
    // wombat matches wombat.* from "wombat*"
    auto wombat = find_instance_field_named(
        delta_i, "Lcom/facebook/redex/test/proguard/Delta$I;.wombat:I");
    ASSERT_NE(wombat, nullptr);
    EXPECT_TRUE(root(wombat));
    EXPECT_TRUE(impl::KeepState::allowobfuscation(wombat));
    // wombat_alpha matches wombat.* from "wombat*"
    auto wombat_alpha = find_instance_field_named(
        delta_i, "Lcom/facebook/redex/test/proguard/Delta$I;.wombat_alpha:I");
    ASSERT_NE(wombat_alpha, nullptr);
    EXPECT_TRUE(root(wombat_alpha));
    EXPECT_TRUE(impl::KeepState::allowobfuscation(wombat_alpha));
    // numbat does not match wombat.* from "wombat*"
    auto numbat = find_instance_field_named(
        delta_i, "Lcom/facebook/redex/test/proguard/Delta$I;.numbat:I");
    ASSERT_NE(nullptr, numbat);
    EXPECT_FALSE(root(numbat));
  }

  { // Test handling of $$ to make sure it does not match against
    // primitive types.
    auto delta_j =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$J;");
    ASSERT_NE(nullptr, delta_j);
    EXPECT_TRUE(root(delta_j));
    // Check for matching using ** *_bear
    // which should match class types but not primitive types or array types.
    // Make sure the field brown_bear is gone.
    auto brown_bear = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.brown_bear:I;");
    ASSERT_EQ(nullptr, brown_bear);
    // Make sure the field back_bear is kept.
    auto black_bear =
        find_instance_field_named(delta_j,
                                  "Lcom/facebook/redex/test/proguard/"
                                  "Delta$J;.black_bear:Ljava/lang/String;");
    ASSERT_NE(nullptr, black_bear);
    EXPECT_TRUE(root(black_bear));
    // grizzly_bear is an array type of a primtive type so should not be kept.
    auto grizzly_bear = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.grizzly_bear:[I;");
    ASSERT_EQ(nullptr, grizzly_bear);
    // polar_bear is an array type of a class type so should not be kept.
    auto polar_bear =
        find_instance_field_named(delta_j,
                                  "Lcom/facebook/redex/test/proguard/"
                                  "Delta$J;.grizzly_bear:[Ljava/lang/String;");
    ASSERT_EQ(nullptr, polar_bear);
    // Check for matches against *** alpha?
    // which should match any type.
    auto alpha0 = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.alpha0:I");
    ASSERT_NE(nullptr, alpha0);
    EXPECT_TRUE(root(alpha0));
    auto alpha1 = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.alpha1:[I");
    ASSERT_NE(nullptr, alpha1);
    EXPECT_TRUE(root(alpha1));
    auto alpha2 = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.alpha2:[[I");
    ASSERT_NE(nullptr, alpha2);
    EXPECT_TRUE(root(alpha2));
    auto alpha3 = find_vmethod_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.alpha3()V");
    ASSERT_EQ(nullptr, alpha3);
    // Check for matches against ** beta*
    // which should only match class types.
    // beta0 is a primitive type, so not kept.
    auto beta0 = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.beta0:I");
    ASSERT_NE(nullptr, beta0);
    EXPECT_FALSE(root(beta0));
    // beta is a class type, so kept
    auto beta = find_instance_field_named(
        delta_j,
        "Lcom/facebook/redex/test/proguard/Delta$J;.beta:Ljava/util/List;");
    ASSERT_NE(nullptr, beta);
    EXPECT_TRUE(root(beta));
    // beta1 is an array of a class type, so not kept.
    auto beta1 = find_instance_field_named(
        delta_j,
        "Lcom/facebook/redex/test/proguard/Delta$J;.beta1:[Ljava/util/List;");
    ASSERT_NE(nullptr, beta1);
    EXPECT_FALSE(root(beta1));
    // Check for matches against public **[] gamma*
    // gamma1 is not kept because int does not match **
    auto gamma1 = find_instance_field_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.gamma1:[I");
    ASSERT_NE(nullptr, gamma1);
    EXPECT_FALSE(root(gamma1));
    // gamma2 is kept because String matches **
    auto gamma2 =
        find_instance_field_named(delta_j,
                                  "Lcom/facebook/redex/test/proguard/"
                                  "Delta$J;.gamma2:[Ljava/lang/String;");
    ASSERT_NE(nullptr, gamma2);
    EXPECT_TRUE(root(gamma2));

    // Test handling of methods.
    auto omega_1 =
        find_vmethod_named(delta_j,
                           "Lcom/facebook/redex/test/proguard/"
                           "Delta$J;.omega:(IZLjava/lang/String;C)I");
    ASSERT_NE(nullptr, omega_1);
    EXPECT_TRUE(root(omega_1));
    auto omega_2 = find_vmethod_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.omega:(S)I");
    ASSERT_NE(nullptr, omega_2);
    EXPECT_TRUE(root(omega_2));
    auto omega_3 = find_vmethod_named(delta_j,
                                      "Lcom/facebook/redex/test/proguard/"
                                      "Delta$J;.omega:(Ljava/lang/String;)I");
    ASSERT_NE(nullptr, omega_3);
    EXPECT_FALSE(root(omega_3));
    auto omega_4 = find_vmethod_named(delta_j,
                                      "Lcom/facebook/redex/test/proguard/"
                                      "Delta$J;.omega:(I)I");
    ASSERT_NE(nullptr, omega_4);
    EXPECT_TRUE(root(omega_4));

    // Check handling of ...
    auto theta_1 =
        find_vmethod_named(delta_j,
                           "Lcom/facebook/redex/test/proguard/"
                           "Delta$J;.theta:(IZLjava/lang/String;C)I");
    ASSERT_NE(nullptr, theta_1);
    EXPECT_TRUE(root(theta_1));
    auto theta_2 = find_vmethod_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.theta:(S)I");
    ASSERT_NE(nullptr, theta_2);
    EXPECT_TRUE(root(theta_2));
    auto theta_3 = find_vmethod_named(delta_j,
                                      "Lcom/facebook/redex/test/proguard/"
                                      "Delta$J;.theta:(Ljava/lang/String;)I");
    ASSERT_NE(nullptr, theta_3);
    EXPECT_TRUE(root(theta_3));

    // Check handling of constructor fors inner class.
    auto init_V =
        find_dmethod_named(delta_j,
                           "Lcom/facebook/redex/test/proguard/"
                           "Delta$J;.<init>:(Lcom/facebook/redex/test/"
                           "proguard/Delta;)V");
    ASSERT_NE(nullptr, init_V);
    EXPECT_TRUE(root(init_V));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(init_V));
    auto init_I = find_dmethod_named(delta_j,
                                     "Lcom/facebook/redex/test/proguard/"
                                     "Delta$J;.<init>(Lcom/facebook/redex/test/"
                                     "proguard/Delta;I)V");
    ASSERT_EQ(nullptr, init_I);
    auto init_S =
        find_dmethod_named(delta_j,
                           "Lcom/facebook/redex/test/proguard/"
                           "Delta$J;.<init>:(Lcom/facebook/redex/test/"
                           "proguard/Delta;Ljava/lang/String;)V");
    ASSERT_NE(nullptr, init_S);
    EXPECT_TRUE(root(init_S));
    EXPECT_FALSE(impl::KeepState::allowobfuscation(init_S));

    // Make sure there are no iotas.
    auto iota_1 = find_vmethod_named(delta_j,
                                     "Lcom/facebook/redex/test/proguard/"
                                     "Delta$J;.iota:(IZLjava/lang/String;C)I");
    ASSERT_NE(nullptr, iota_1);
    EXPECT_FALSE(root(iota_1));
    auto iota_2 = find_vmethod_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.iota(S)I");
    ASSERT_EQ(nullptr, iota_2);
    auto iota_3 = find_vmethod_named(delta_j,
                                     "Lcom/facebook/redex/test/proguard/"
                                     "Delta$J;.iota:(Ljava/lang/String;)I");
    ASSERT_NE(nullptr, iota_3);
    EXPECT_FALSE(root(iota_3));

    // Checking handling of % matches against void
    auto zeta0 = find_vmethod_named(
        delta_j, "Lcom/facebook/redex/test/proguard/Delta$J;.zeta0:()V");
    ASSERT_NE(nullptr, zeta0);
    EXPECT_TRUE(root(zeta0));
    auto zeta1 = find_vmethod_named(delta_j,
                                    "Lcom/facebook/redex/test/proguard/"
                                    "Delta$J;.zeta1:()Ljava/lang/String;");
    ASSERT_NE(nullptr, zeta1);
    EXPECT_FALSE(root(zeta1));
  }

  { // Check handling of annotations.
    auto delta_k =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$K;");
    ASSERT_NE(nullptr, delta_k);
    EXPECT_TRUE(root(delta_k));
    auto alpha = find_instance_field_named(
        delta_k, "Lcom/facebook/redex/test/proguard/Delta$K;.alpha:I");
    ASSERT_NE(nullptr, alpha);
    EXPECT_FALSE(root(alpha));
    auto beta = find_instance_field_named(
        delta_k, "Lcom/facebook/redex/test/proguard/Delta$K;.beta:I");
    ASSERT_NE(nullptr, beta);
    EXPECT_TRUE(root(beta));
    auto gamma = find_vmethod_named(
        delta_k, "Lcom/facebook/redex/test/proguard/Delta$K;.gamma:()V");
    ASSERT_NE(nullptr, gamma);
    EXPECT_FALSE(root(gamma));
    auto omega = find_vmethod_named(
        delta_k, "Lcom/facebook/redex/test/proguard/Delta$K;.omega:()V");
    ASSERT_NE(nullptr, omega);
    EXPECT_TRUE(root(omega));
  }

  { // Check handling of conflicting access modifiers.
    auto delta_l =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$L;");
    ASSERT_NE(nullptr, delta_l);
    EXPECT_TRUE(root(delta_l));
    auto alpha0 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.alpha0:()V");
    ASSERT_NE(nullptr, alpha0);
    EXPECT_TRUE(root(alpha0));
    auto alpha1 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.alpha1:()V");
    ASSERT_NE(nullptr, alpha1);
    EXPECT_TRUE(root(alpha1));
    auto alpha2 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;alpha2:()V");
    ASSERT_EQ(nullptr, alpha2);

    auto beta0 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.beta0:()V");
    ASSERT_NE(nullptr, beta0);
    EXPECT_TRUE(root(beta0));
    auto beta1 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.beta1:()V");
    ASSERT_NE(nullptr, beta1);
    EXPECT_TRUE(root(beta1));
    auto beta2 = find_dmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.beta2:()V");
    ASSERT_NE(nullptr, beta2);
    EXPECT_TRUE(root(beta2));

    auto gamma0 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.gamma0:()V");
    ASSERT_NE(nullptr, gamma0);
    EXPECT_TRUE(root(gamma0));
    auto gamma1 = find_vmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.gamma1:()V");
    ASSERT_NE(nullptr, gamma1);
    EXPECT_TRUE(root(gamma1));
    auto gamma2 = find_dmethod_named(
        delta_l, "Lcom/facebook/redex/test/proguard/Delta$L;.gamma2:()V");
    ASSERT_NE(nullptr, gamma2);
    EXPECT_TRUE(root(gamma2));

    // Check handling of extends for nested classes.
    auto delta_m =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$M;");
    ASSERT_NE(nullptr, delta_m);
    EXPECT_TRUE(root(delta_m));
    auto delta_n =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$N;");
    ASSERT_NE(nullptr, delta_n);
    EXPECT_TRUE(root(delta_n));
    auto delta_o =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$O;");
    ASSERT_NE(nullptr, delta_o);
    EXPECT_FALSE(root(delta_o));

    auto delta_p =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$P;");
    ASSERT_NE(nullptr, delta_p);
    EXPECT_TRUE(root(delta_p));

    // Check direct extends into dependent jar.
    auto delta_q1 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$Q1;");
    ASSERT_NE(nullptr, delta_q1);
    EXPECT_TRUE(root(delta_q1));
    // Check deeper extends into dependent jar.
    auto delta_q2 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$Q2;");
    ASSERT_NE(nullptr, delta_q2);
    EXPECT_TRUE(root(delta_q2));

    // Check DontKillMe
    auto delta_r =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$R;");
    ASSERT_NE(nullptr, delta_r);

    // Check handling of extends for classes with annotation filters.
    auto delta_s0 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$S0;");
    ASSERT_NE(nullptr, delta_s0);
    EXPECT_TRUE(root(delta_s0));
    auto delta_s1 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$S1;");
    ASSERT_NE(nullptr, delta_s1);
    EXPECT_FALSE(root(delta_s1));

    // Check assumenosideeffects
    auto delta_u =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$U;");
    ASSERT_NE(nullptr, delta_u);
    EXPECT_TRUE(root(delta_u));
    auto logger = find_vmethod_named(
        delta_u, "Lcom/facebook/redex/test/proguard/Delta$U;.logger:()V");
    ASSERT_NE(nullptr, logger);
    EXPECT_FALSE(root(logger));
    EXPECT_TRUE(assumenosideeffects(logger));
    auto mutator = find_vmethod_named(
        delta_u, "Lcom/facebook/redex/test/proguard/Delta$U;.mutator:()V");
    ASSERT_NE(nullptr, mutator);
    EXPECT_TRUE(root(mutator));
    EXPECT_FALSE(assumenosideeffects(mutator));

    // Check keepclasseswithmembers on Delta.X class
    auto delta_x =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$X;");
    ASSERT_NE(nullptr, delta_x);

    auto delta_x_x1 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$X$X1;");
    ASSERT_NE(nullptr, delta_x_x1);
    EXPECT_TRUE(root(delta_x_x1));
    auto delta_x_x1_init = find_dmethod_named(
        delta_x_x1,
        "Lcom/facebook/redex/test/proguard/Delta$X$X1;.<init>:(Lcom/facebook/"
        "redex/test/proguard/Delta$X;)V");
    ASSERT_NE(nullptr, delta_x_x1_init);
    EXPECT_TRUE(root(delta_x_x1_init));

    auto delta_x_x2 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$X$X2;");
    ASSERT_NE(nullptr, delta_x_x2);
    EXPECT_FALSE(root(delta_x_x2));
  }

  { // keepclasseswithmembers test
    auto delta_v =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$V;");
    ASSERT_NE(nullptr, delta_v);
    EXPECT_TRUE(root(delta_v));
    auto goat = find_instance_field_named(
        delta_v,
        "Lcom/facebook/redex/test/proguard/Delta$V;.goat:Lcom/facebook/redex/"
        "test/proguard/Delta$VT;");
    ASSERT_NE(nullptr, goat);
    EXPECT_TRUE(root(goat));
    auto sheep = find_instance_field_named(
        delta_v,
        "Lcom/facebook/redex/test/proguard/Delta$V;.sheep:Lcom/facebook/redex/"
        "test/proguard/Delta$VT;");
    ASSERT_NE(nullptr, sheep);
    EXPECT_TRUE(root(sheep));
    auto lama = find_instance_field_named(
        delta_v, "Lcom/facebook/redex/test/proguard/Delta$V;.lama:I");
    ASSERT_NE(nullptr, lama);
    EXPECT_FALSE(root(lama));
  }

  { // More keepclasseswithmembers for the E0, E1, .. E7 classes
    auto delta_e7 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$E7;");
    ASSERT_NE(nullptr, delta_e7);
    EXPECT_TRUE(root(delta_e7));
    auto e7_crab = find_instance_field_named(
        delta_e7, "Lcom/facebook/redex/test/proguard/Delta$E7;.crab:I");
    ASSERT_NE(nullptr, e7_crab);
    EXPECT_TRUE(root(e7_crab));
    auto e7_seahorse = find_instance_field_named(
        delta_e7, "Lcom/facebook/redex/test/proguard/Delta$E7;.seahorse:I");
    ASSERT_NE(nullptr, e7_seahorse);
    EXPECT_TRUE(root(e7_seahorse));
    auto e7_octopus = find_instance_field_named(
        delta_e7, "Lcom/facebook/redex/test/proguard/Delta$E7;.octopus:I");
    ASSERT_NE(nullptr, e7_octopus);
    EXPECT_FALSE(root(e7_octopus));
    auto e7_shark = find_vmethod_named(
        delta_e7, "Lcom/facebook/redex/test/proguard/Delta$E7;.shark:()I");
    ASSERT_NE(nullptr, e7_shark);
    EXPECT_TRUE(root(e7_shark));
    auto e7_tuna1 = find_vmethod_named(
        delta_e7, "Lcom/facebook/redex/test/proguard/Delta$E7;.tuna1:()I");
    ASSERT_NE(nullptr, e7_tuna1);
    EXPECT_TRUE(root(e7_tuna1));
    auto e7_tuna2 = find_vmethod_named(
        delta_e7, "Lcom/facebook/redex/test/proguard/Delta$E7;.tuna2:()I");
    ASSERT_NE(nullptr, e7_tuna2);
    EXPECT_TRUE(root(e7_tuna2));
  }

  { // keepnames test
    auto delta_w =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$W;");
    ASSERT_NE(nullptr, delta_w);
    EXPECT_FALSE(root(delta_w));
    EXPECT_FALSE(can_rename(delta_w));

    auto delta_s3 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$S3;");
    ASSERT_NE(nullptr, delta_s3);
    EXPECT_FALSE(root(delta_s3));
    EXPECT_TRUE(can_rename(delta_s3));

    auto delta_s4 = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Delta$S4;");
    ASSERT_NE(nullptr, delta_s4);
    EXPECT_FALSE(root(delta_s4));
    EXPECT_FALSE(can_rename(delta_s4));
  }

  { // Check extends
    auto epsilon =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Epsilon;");
    ASSERT_NE(nullptr, epsilon);
    auto zeta =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Zeta;");
    ASSERT_NE(nullptr, zeta);
    EXPECT_TRUE(root(zeta));
  }

  { // Implementation checks
    auto theta_a =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Theta$A;");
    ASSERT_NE(nullptr, theta_a);
    EXPECT_TRUE(root(theta_a));
    auto theta_b =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Theta$B;");
    ASSERT_NE(nullptr, theta_b);
    EXPECT_TRUE(root(theta_b));
  }

  { // keepclassmembers tests
    auto iota =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Iota;");
    ASSERT_NE(nullptr, iota);

    auto iota_alpha = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Iota$Alpha;");
    ASSERT_NE(nullptr, iota_alpha);
    EXPECT_TRUE(root(iota_alpha));
    auto alpha_encode = find_vmethod_named(iota_alpha,
                                           "Lcom/facebook/redex/test/proguard/"
                                           "Iota$Alpha;.encode:(I)I");
    ASSERT_NE(nullptr, alpha_encode);
    EXPECT_TRUE(root(alpha_encode));

    auto alpha_decode = find_vmethod_named(iota_alpha,
                                           "Lcom/facebook/redex/test/proguard/"
                                           "Iota$Alpha;.decode:(I)I");
    ASSERT_NE(nullptr, alpha_decode);
    EXPECT_TRUE(root(alpha_decode));

    auto alpha_wombat = find_vmethod_named(iota_alpha,
                                           "Lcom/facebook/redex/test/proguard/"
                                           "Iota$Alpha;.wombat:(I)I");
    ASSERT_EQ(nullptr, alpha_wombat);

    auto iota_beta = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Iota$Beta;");
    ASSERT_NE(nullptr, iota_beta);
    EXPECT_FALSE(root(iota_beta));

    auto iota_MySerializable = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Iota$MySerializable;");
    ASSERT_NE(nullptr, iota_MySerializable);

    auto iota_someother = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Iota$SomeOther;");
    ASSERT_NE(nullptr, iota_someother);
    EXPECT_TRUE(root(iota_someother));
    auto iota_someother_init = find_dmethod_named(
        iota_someother,
        "Lcom/facebook/redex/test/proguard/Iota$SomeOther;.<init>:(Lcom/"
        "facebook/redex/test/proguard/Iota;)V");
    ASSERT_NE(nullptr, iota_someother_init);

    // Iota.Gamma does not have a keep directive, but it is indirectly used
    // in the constructor for SomeOther.
    auto iota_gamma = find_class_named(
        classes, "Lcom/facebook/redex/test/proguard/Iota$Gamma;");
    ASSERT_NE(nullptr, iota_gamma);
    EXPECT_FALSE(root(iota_gamma));

    auto gamma_encode = find_vmethod_named(iota_gamma,
                                           "Lcom/facebook/redex/test/proguard/"
                                           "Iota$Gamma;.encode:(I)I");
    ASSERT_NE(nullptr, gamma_encode);
    EXPECT_TRUE(root(gamma_encode));

    auto gamma_decode = find_vmethod_named(iota_gamma,
                                           "Lcom/facebook/redex/test/proguard/"
                                           "Iota$Gamma;.decode:(I)I");
    ASSERT_NE(nullptr, gamma_decode);
    EXPECT_TRUE(root(gamma_decode));

    auto gamma_numbat = find_vmethod_named(iota_gamma,
                                           "Lcom/facebook/redex/test/proguard/"
                                           "Iota$Gamma;.numbat:(I)I");
    ASSERT_EQ(nullptr, gamma_numbat);

    { // keepclasseswithmembers tests
      auto omega =
          find_class_named(classes, "Lcom/facebook/redex/test/proguard/Omega;");
      ASSERT_NE(nullptr, omega);

      auto omega_alpha = find_class_named(
          classes, "Lcom/facebook/redex/test/proguard/Omega$Alpha;");
      ASSERT_NE(nullptr, omega_alpha);
      EXPECT_TRUE(root(omega_alpha));
      auto omega_alpha_red = find_vmethod_named(
          omega_alpha,
          "Lcom/facebook/redex/test/proguard/Omega$Alpha;.red:()V");
      ASSERT_NE(nullptr, omega_alpha_red);
      EXPECT_TRUE(root(omega_alpha_red));
      auto omega_alpha_green0 = find_vmethod_named(
          omega_alpha,
          "Lcom/facebook/redex/test/proguard/Omega$Alpha;.green0:()V");
      ASSERT_NE(nullptr, omega_alpha_green0);
      EXPECT_TRUE(root(omega_alpha_green0));
      auto omega_alpha_green1 = find_vmethod_named(
          omega_alpha,
          "Lcom/facebook/redex/test/proguard/Omega$Alpha;.green1:()V");
      ASSERT_NE(nullptr, omega_alpha_green1);
      EXPECT_TRUE(root(omega_alpha_green1));
      auto omega_alpha_blue = find_vmethod_named(
          omega_alpha,
          "Lcom/facebook/redex/test/proguard/Omega$Alpha;.blue:()V");
      ASSERT_NE(nullptr, omega_alpha_blue);
      EXPECT_FALSE(root(omega_alpha_blue));

      auto omega_beta = find_class_named(
          classes, "Lcom/facebook/redex/test/proguard/Omega$Beta;");
      ASSERT_NE(nullptr, omega_beta);
      EXPECT_FALSE(root(omega_beta));

      auto omega_gamma = find_class_named(
          classes, "Lcom/facebook/redex/test/proguard/Omega$Gamma;");
      ASSERT_NE(nullptr, omega_gamma);
      EXPECT_FALSE(root(omega_gamma));
    }
  }
  { // check some keep rules are not used
    EXPECT_EQ(unused_rules.size(), 4);
    std::vector<std::string> unused_rule_strings;
    for (const keep_rules::KeepSpec* keep_rule : unused_rules) {
      unused_rule_strings.push_back(keep_rules::show_keep(*keep_rule, false));
    }
    EXPECT_THAT(
        unused_rule_strings,
        ::testing::UnorderedElementsAre(
            "-keep android.support.test.runner.AndroidJUnitRunner {  (...)V "
            "<init>(); }",
            "-keepclassmembers com.facebook.redex.test.proguard.Delta$U { ()V "
            "logger(); }",
            "-keepclasseswithmembernames * { native  *(); }",
            "-keep androidx.test.runner.AndroidJUnitRunner {  (...)V "
            "<init>(); }"));
  }
}
