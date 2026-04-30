/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Dex037TestHelper.h"

TEST_F(Dex037Test, TestIGreeterHasCorrectStructure) {
  verify_TestIGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestIFormalGreeterHasCorrectStructure) {
  verify_TestIFormalGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestSimpleGreeterHasCorrectStructure) {
  verify_TestSimpleGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestComplexGreeterHasCorrectStructure) {
  verify_TestComplexGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestCustomGreeterHasCorrectStructure) {
  verify_TestCustomGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestDoubleCustomGreeterHasCorrectStructure) {
  verify_TestDoubleCustomGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestSuperCallingGreeterHasCorrectStructure) {
  verify_TestSuperCallingGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestAbstractGreeterHasCorrectStructure) {
  verify_TestAbstractGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestConcreteGreeterHasCorrectStructure) {
  verify_TestConcreteGreeterHasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerHasCorrectStructure) {
  verify_TestILayerHasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerOverrideHasCorrectStructure) {
  verify_TestILayerOverrideHasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerOverrideChildHasCorrectStructure) {
  verify_TestILayerOverrideChildHasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerOverrideChild2HasCorrectStructure) {
  verify_TestILayerOverrideChild2HasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerNoOverrideHasCorrectStructure) {
  verify_TestILayerNoOverrideHasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerConflictHasCorrectStructure) {
  verify_TestILayerConflictHasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerLast1HasCorrectStructure) {
  verify_TestILayerLast1HasCorrectStructure();
}

TEST_F(Dex037Test, TestILayerLast2HasCorrectStructure) {
  verify_TestILayerLast2HasCorrectStructure();
}

TEST_F(Dex037Test, TestLayer1HasCorrectStructure) {
  verify_TestLayer1HasCorrectStructure();
}

TEST_F(Dex037Test, TestLayer2HasCorrectStructure) {
  verify_TestLayer2HasCorrectStructure();
}

TEST_F(Dex037Test, TestLayer3HasCorrectStructure) {
  verify_TestLayer3HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer1HasCorrectStructure) {
  verify_TestFinalLayer1HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer2HasCorrectStructure) {
  verify_TestFinalLayer2HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer3HasCorrectStructure) {
  verify_TestFinalLayer3HasCorrectStructure();
}

TEST_F(Dex037Test, TestMidLayerHasCorrectStructure) {
  verify_TestMidLayerHasCorrectStructure();
}

TEST_F(Dex037Test, TestMidLayer2HasCorrectStructure) {
  verify_TestMidLayer2HasCorrectStructure();
}

TEST_F(Dex037Test, TestMidLayer3HasCorrectStructure) {
  verify_TestMidLayer3HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer4HasCorrectStructure) {
  verify_TestFinalLayer4HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer4_2HasCorrectStructure) {
  verify_TestFinalLayer4_2HasCorrectStructure();
}

TEST_F(Dex037Test, TestTopLayerHasCorrectStructure) {
  verify_TestTopLayerHasCorrectStructure();
}

TEST_F(Dex037Test, TestMidLayer4HasCorrectStructure) {
  verify_TestMidLayer4HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer4_3HasCorrectStructure) {
  verify_TestFinalLayer4_3HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer5HasCorrectStructure) {
  verify_TestFinalLayer5HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer6HasCorrectStructure) {
  verify_TestFinalLayer6HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer7HasCorrectStructure) {
  verify_TestFinalLayer7HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer8HasCorrectStructure) {
  verify_TestFinalLayer8HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer9HasCorrectStructure) {
  verify_TestFinalLayer9HasCorrectStructure();
}

TEST_F(Dex037Test, TestFinalLayer10HasCorrectStructure) {
  verify_TestFinalLayer10HasCorrectStructure();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualOnClass) {
  verify_TestResolveMethodInvokeVirtualOnClass();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceOnInterface) {
  verify_TestResolveMethodInvokeInterfaceOnInterface();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualOnOverriddenMethod) {
  verify_TestResolveMethodInvokeVirtualOnOverriddenMethod();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualOnParentClass) {
  verify_TestResolveMethodInvokeVirtualOnParentClass();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceOnOverriddenMethod) {
  verify_TestResolveMethodInvokeInterfaceOnOverriddenMethod();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualWithOverride) {
  verify_TestResolveMethodInvokeVirtualWithOverride();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualOnMultipleInterfaces) {
  verify_TestResolveMethodInvokeVirtualOnMultipleInterfaces();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceOnMultipleInterfaces) {
  verify_TestResolveMethodInvokeInterfaceOnMultipleInterfaces();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceOnMultipleInterfaces2) {
  verify_TestResolveMethodInvokeInterfaceOnMultipleInterfaces2();
}

TEST_F(Dex037Test, TestResolveMethodInvokeSuperInterface) {
  verify_TestResolveMethodInvokeSuperInterface();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceInvokeSuperInterface) {
  verify_TestResolveMethodInvokeInterfaceInvokeSuperInterface();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceInvokeSuperInterface2) {
  verify_TestResolveMethodInvokeInterfaceInvokeSuperInterface2();
}

TEST_F(Dex037Test, TestResolveMethodInvokeStaticInterface) {
  verify_TestResolveMethodInvokeStaticInterface();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualOnConcreteClass) {
  verify_TestResolveMethodInvokeVirtualOnConcreteClass();
}

TEST_F(Dex037Test, TestResolveMethodInvokeVirtualOnAbstractClass) {
  verify_TestResolveMethodInvokeVirtualOnAbstractClass();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceOnConcreteClass) {
  verify_TestResolveMethodInvokeInterfaceOnConcreteClass();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceOnReferences) {
  verify_TestResolveMethodInvokeInterfaceOnReferences();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeMultipleInterfaces1) {
  verify_TestResolveMethodLayeredInvokeMultipleInterfaces1();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeMultipleInterfaces2) {
  verify_TestResolveMethodLayeredInvokeMultipleInterfaces2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeMultipleInterfaces3) {
  verify_TestResolveMethodLayeredInvokeMultipleInterfaces3();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeClassAndInterface1) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface1();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeClassAndInterface2) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeClassAndInterface2_2) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface2_2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeClassAndInterface3) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface3();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeClassAndInterface3_2) {
  verify_TestResolveMethodLayeredInvokeClassAndInterface3_2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeWithRedundantInterfaces) {
  verify_TestResolveMethodLayeredInvokeWithRedundantInterfaces();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeWithClassMultipleInterface) {
  verify_TestResolveMethodLayeredInvokeWithClassMultipleInterface();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeWithDiamondInterface1) {
  verify_TestResolveMethodLayeredInvokeWithDiamondInterface1();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeWithDiamondInterface2) {
  verify_TestResolveMethodLayeredInvokeWithDiamondInterface2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeSuperWithInheritedDefault) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeSuperWithInheritedDefault2) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_2) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_2();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_3) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_3();
}

TEST_F(Dex037Test, TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_4) {
  verify_TestResolveMethodLayeredInvokeSuperWithInheritedDefault2_4();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceVirtualDifference) {
  verify_TestResolveMethodInvokeInterfaceVirtualDifference();
}

TEST_F(Dex037Test, TestResolveMethodInvokeInterfaceVirtualDifference2) {
  verify_TestResolveMethodInvokeInterfaceVirtualDifference2();
}
