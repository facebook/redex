/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Dex037TestHelper.h"

// Kotlin version of Dex037 tests
// This test suite verifies that Kotlin-compiled code produces the same
// DEX 037 structure as Java-compiled code for default interface methods
class Dex037KotlinTest : public Dex037Test {};

// All test cases reuse the same verification logic from Dex037TestHelper.h
// These tests verify that Kotlin produces the same DEX 037 structure as Java

TEST_F(Dex037KotlinTest, TestIGreeterHasCorrectStructure) {
  verify_TestIGreeterHasCorrectStructure(true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestIFormalGreeterHasCorrectStructure) {
  verify_TestIFormalGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestSimpleGreeterHasCorrectStructure) {
  verify_TestSimpleGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestComplexGreeterHasCorrectStructure) {
  verify_TestComplexGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestCustomGreeterHasCorrectStructure) {
  verify_TestCustomGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestDoubleCustomGreeterHasCorrectStructure) {
  verify_TestDoubleCustomGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestSuperCallingGreeterHasCorrectStructure) {
  verify_TestSuperCallingGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestAbstractGreeterHasCorrectStructure) {
  verify_TestAbstractGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestConcreteGreeterHasCorrectStructure) {
  verify_TestConcreteGreeterHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerHasCorrectStructure) {
  verify_TestILayerHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerOverrideHasCorrectStructure) {
  verify_TestILayerOverrideHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerOverrideChildHasCorrectStructure) {
  verify_TestILayerOverrideChildHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerOverrideChild2HasCorrectStructure) {
  verify_TestILayerOverrideChild2HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerNoOverrideHasCorrectStructure) {
  verify_TestILayerNoOverrideHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerConflictHasCorrectStructure) {
  verify_TestILayerConflictHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerLast1HasCorrectStructure) {
  verify_TestILayerLast1HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestILayerLast2HasCorrectStructure) {
  verify_TestILayerLast2HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestLayer1HasCorrectStructure) {
  verify_TestLayer1HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestLayer2HasCorrectStructure) {
  verify_TestLayer2HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestLayer3HasCorrectStructure) {
  verify_TestLayer3HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer1HasCorrectStructure) {
  verify_TestFinalLayer1HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer2HasCorrectStructure) {
  verify_TestFinalLayer2HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer3HasCorrectStructure) {
  verify_TestFinalLayer3HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestMidLayerHasCorrectStructure) {
  verify_TestMidLayerHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestMidLayer2HasCorrectStructure) {
  verify_TestMidLayer2HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestMidLayer3HasCorrectStructure) {
  verify_TestMidLayer3HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer4HasCorrectStructure) {
  verify_TestFinalLayer4HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer4_2HasCorrectStructure) {
  verify_TestFinalLayer4_2HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestTopLayerHasCorrectStructure) {
  verify_TestTopLayerHasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestMidLayer4HasCorrectStructure) {
  verify_TestMidLayer4HasCorrectStructure(true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestFinalLayer4_3HasCorrectStructure) {
  verify_TestFinalLayer4_3HasCorrectStructure(true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestFinalLayer5HasCorrectStructure) {
  verify_TestFinalLayer5HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer6HasCorrectStructure) {
  verify_TestFinalLayer6HasCorrectStructure(true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestFinalLayer7HasCorrectStructure) {
  verify_TestFinalLayer7HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer8HasCorrectStructure) {
  verify_TestFinalLayer8HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer9HasCorrectStructure) {
  verify_TestFinalLayer9HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestFinalLayer10HasCorrectStructure) {
  verify_TestFinalLayer10HasCorrectStructure();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualOnClass) {
  verify_TestResolveMethodInvokeVirtualOnClass();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceOnInterface) {
  verify_TestResolveMethodInvokeInterfaceOnInterface();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualOnOverriddenMethod) {
  verify_TestResolveMethodInvokeVirtualOnOverriddenMethod();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualOnParentClass) {
  verify_TestResolveMethodInvokeVirtualOnParentClass();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceOnOverriddenMethod) {
  verify_TestResolveMethodInvokeInterfaceOnOverriddenMethod();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualWithOverride) {
  verify_TestResolveMethodInvokeVirtualWithOverride();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualOnMultipleInterfaces) {
  verify_TestResolveMethodInvokeVirtualOnMultipleInterfaces();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceOnMultipleInterfaces) {
  verify_TestResolveMethodInvokeInterfaceOnMultipleInterfaces();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodInvokeInterfaceOnMultipleInterfaces2) {
  verify_TestResolveMethodInvokeInterfaceOnMultipleInterfaces2();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeSuperInterface) {
  verify_TestResolveMethodInvokeSuperInterface();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceInvokeSuperInterface) {
  verify_TestResolveMethodInvokeInterfaceInvokeSuperInterface();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodInvokeInterfaceInvokeSuperInterface2) {
  verify_TestResolveMethodInvokeInterfaceInvokeSuperInterface2();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeStaticInterface) {
  verify_TestResolveMethodInvokeStaticInterface(true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualOnConcreteClass) {
  verify_TestResolveMethodInvokeVirtualOnConcreteClass();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeVirtualOnAbstractClass) {
  verify_TestResolveMethodInvokeVirtualOnAbstractClass();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceOnConcreteClass) {
  verify_TestResolveMethodInvokeInterfaceOnConcreteClass();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceOnReferences) {
  verify_TestResolveMethodInvokeInterfaceOnReferences();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeMultipleInterfaces1) {
  verify_TestResolveMethodLayeredInvokeMultipleInterfaces1();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeMultipleInterfaces2) {
  verify_TestResolveMethodLayeredInvokeMultipleInterfaces2();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeMultipleInterfaces3) {
  verify_TestResolveMethodLayeredInvokeMultipleInterfaces3();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeClassAndInterface1) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface1();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeClassAndInterface2) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface2();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeClassAndInterface2_2) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface2_2();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeClassAndInterface3) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface3(true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeClassAndInterface3_2) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface3_2();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeWithRedundantInterfaces) {
  verify_TestResolveMethodLayeredInvokeWithRedundantInterfaces();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeWithClassMultipleInterface) {
  verify_TestResolveMethodLayeredInvokeWithClassMultipleInterface(
      true /*is_kotlin=*/);
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeWithDiamondInterface1) {
  verify_TestResolveMethodLayeredInvokeWithDiamondInterface1();
}

TEST_F(Dex037KotlinTest, TestResolveMethodLayeredInvokeWithDiamondInterface2) {
  verify_TestResolveMethodLayeredInvokeWithDiamondInterface2();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeSuperWithInheritedDefault) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeSuperWithInheritedDefault2) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_2) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_2();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_3) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_3();
}

TEST_F(Dex037KotlinTest,
       TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_4) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_4();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceVirtualDifference) {
  verify_TestResolveMethodInvokeInterfaceVirtualDifference();
}

TEST_F(Dex037KotlinTest, TestResolveMethodInvokeInterfaceVirtualDifference2) {
  verify_TestResolveMethodInvokeInterfaceVirtualDifference2();
}
