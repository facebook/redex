/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Kotlin class is used to test default interface methods (Java 8 feature). It tests various
 * scenarios including:
 * - Default interface methods
 * - Static interface methods (companion objects in Kotlin)
 * - Class implementing interface with default method
 * - Class overriding default method
 * - Multiple interfaces with default methods
 */
package com.facebook.redextest

import org.assertj.core.api.Assertions.assertThat
import org.junit.Test

class Dex037 {

  // Interface with default methods
  interface IGreeter {
    private fun emptyString(): String = ""

    fun greet(): String = emptyString() + "Hello from IGreeter"

    fun farewell(): String = "Goodbye from IGreeter"

    fun nestedgreet(): String = greet()

    fun othermsg(): String = "othermsg from IGreeter"

    fun nonDefaultString(): String

    companion object {
      @JvmStatic fun staticGreet(): String = "Static hello from IGreeter"
    }
  }

  // Interface with another default method
  interface IFormalGreeter {
    fun greet(): String = "Hello from IFormalGreeter"
  }

  // Class implementing interface without overriding default method
  open inner class SimpleGreeter : IGreeter {
    // Uses default implementations from Greeter except nonDefaultString
    // that need implementation
    override fun nonDefaultString(): String {
      return "nonDefaultString from SimpleGreeter"
    }
  }

  // Class extending class implementing interface overriding default method
  inner class ComplexGreeter : SimpleGreeter() {
    override fun greet(): String {
      return "Hello from ComplexGreeter"
    }
  }

  // Class implementing interface and overriding default method
  inner class CustomGreeter : IGreeter {
    override fun greet(): String {
      return "Hello from CustomGreeter"
    }

    override fun nonDefaultString(): String {
      return "nonDefaultString from CustomGreeter"
    }
    // farewell() still uses default implementation
  }

  // Class implementing multiple interfaces with same default method signature
  inner class DoubleCustomGreeter : IGreeter, IFormalGreeter {
    // Must override greet() to resolve conflict
    override fun greet(): String {
      return "Hello from DoubleCustomGreeter"
    }

    override fun nonDefaultString(): String {
      return "nonDefaultString from DoubleCustomGreeter"
    }
  }

  open inner class Parent {
    open fun foo(): String = "ParentClass"
  }

  // Class calling super interface default method
  inner class SuperCallingGreeter : Parent(), IGreeter, IFormalGreeter {
    override fun greet(): String {
      return super<IFormalGreeter>.greet() + " and " + super.foo() + " and SuperCallingGreeter"
    }

    fun wrapGreet(): String {
      return super<IGreeter>.greet() + " and " + super.foo() + " and SuperCallingGreeter"
    }

    override fun nonDefaultString(): String {
      return "nonDefaultString from SuperCallingGreeter"
    }
  }

  // Abstract class implementing interface
  abstract inner class AbstractGreeter : IGreeter {
    abstract fun additionalGreet(): String

    abstract override fun greet(): String
  }

  // Concrete class extending abstract class
  inner class ConcreteGreeter : AbstractGreeter() {
    override fun greet(): String = "Hello from ConcreteGreeter"

    override fun additionalGreet(): String {
      return "Additional hello from ConcreteGreeter"
    }

    override fun farewell(): String {
      return "Goodbye from ConcreteGreeter"
    }

    override fun nonDefaultString(): String {
      return "nonDefaultString from ConcreteGreeter"
    }
  }

  interface ILayer {
    fun layer(): String = "first"
  }

  interface ILayerOverride : ILayer {
    override fun layer(): String = "second"
  }

  interface ILayerOverrideChild : ILayerOverride

  interface ILayerOverrideChild2 : ILayer, ILayerOverride

  interface ILayerNoOverride : ILayer

  // An interface to create default interface conflict
  interface ILayerConflict {
    fun layer(): String = "third"
  }

  // Testing which layer will be resolved to, does it depend on interface
  // sequence in FinalLayer7 and FinalLayer8
  interface ILayerLast1 : ILayerOverride, ILayerNoOverride

  interface ILayerLast2 : ILayerNoOverride, ILayerOverride

  // The following three is testing resolver can find the
  // correct method regardless of the order of interfaces,
  // with and without the override in second layer of interface
  open inner class Layer1 : ILayerOverride, ILayer

  open inner class Layer2 : ILayerNoOverride, ILayer

  open inner class Layer3 : ILayer, ILayerOverride

  inner class FinalLayer1 : Layer1() {
    fun getLayer(): String = super.layer()
  }

  inner class FinalLayer2 : Layer2() {
    fun getLayer(): String = super.layer()
  }

  inner class FinalLayer3 : Layer3() {
    fun getLayer(): String = super.layer()
  }

  open inner class MidLayer : ILayer

  open inner class MidLayer2 : ILayerNoOverride, ILayer, ILayerLast1, ILayerOverride

  open inner class MidLayer3 : ILayer {
    override fun layer(): String = "four"
  }

  open inner class TopLayer {
    open fun layer(): String = "top"
  }

  /**
   * Following is allowed in java, because java follows a strict "Class Wins" rule for conflict
   * resolution. But this is not allowed for kotlin, such compile error will be thrown: class
   * 'MidLayer4' must override public open fun layer(): String defined in
   * com.facebook.redextest.Dex037.TopLayer because it inherits many implementations of it
   *
   * open inner class MidLayer4 : TopLayer(), ILayer
   */
  open inner class MidLayer4 : TopLayer(), ILayer {
    override fun layer(): String = super<TopLayer>.layer()
  }

  open inner class MidLayer5 : ILayerOverride

  // Test which method get resolved to when a class has a super class getting
  // layer method from its interface's default method, and there is conflicting
  // method in its interface
  inner class FinalLayer4 : MidLayer(), ILayerOverride {
    fun getLayer(): String = super<MidLayer>.layer()
  }

  // Similar to above, but what if the direct super class and interface all
  // don't have a method and they all get it from their interface's default
  // method
  inner class FinalLayer4_2 : MidLayer(), ILayerOverrideChild {
    /**
     * Following is allowed in java, but this is not allowed for kotlin, such compile error will be
     * thrown: many supertypes available, please specify the one you mean in angle brackets, e.g.
     * 'super<Foo>'
     *
     * fun getLayer(): String = super.layer()
     */
    fun getLayer(): String = super<MidLayer>.layer()
  }

  /**
   * Following is allowed in java, because java follows a "Class Wins" rule for conflict resolution.
   * But this is not allowed for kotlin, such compile error will be thrown: class 'FinalLayer4_3'
   * must override public open fun layer(): String defined in
   * com.facebook.redextest.Dex037.MidLayer4 because it inherits many implementations of it
   *
   * inner class FinalLayer4_3 : MidLayer4(), ILayerConflict { fun getLayer(): String =
   * super<MidLayer4>.layer() }
   */
  inner class FinalLayer4_3 : MidLayer4(), ILayerConflict {
    fun getLayer(): String = super<MidLayer4>.layer()

    override fun layer(): String = super<MidLayer4>.layer()
  }

  // Test which method get resolved to when a class has a super class
  // that has a complicated interface hierachy
  inner class FinalLayer5 : MidLayer2() {
    fun getLayer(): String = super.layer()
  }

  /**
   * Following is allowed in java, because java follows a "Class Wins" rule for conflict resolution.
   * But this is not allowed for kotlin, such compile error will be thrown: class 'FinalLayer4_3'
   * must override public open fun layer(): String defined in
   * com.facebook.redextest.Dex037.MidLayer4 because it inherits many implementations of it
   *
   * and many supertypes available, please specify the one you mean in angle brackets, e.g.
   * 'super<Foo>'
   *
   * inner class FinalLayer6 : MidLayer3(), ILayer, ILayerConflict { fun getLayer(): String =
   * super.layer() }
   */
  inner class FinalLayer6 : MidLayer3(), ILayer, ILayerConflict {
    fun getLayer(): String = super<MidLayer3>.layer()

    override fun layer(): String = super<MidLayer3>.layer()
  }

  // Testing which layer will be resolved to, does it depend on interface
  // sequence
  inner class FinalLayer7 : ILayerLast1 {
    fun getLayer(): String = layer()
  }

  inner class FinalLayer8 : ILayerLast2 {
    fun getLayer(): String = layer()
  }

  // This is to check if resolving invoke-super for default interface
  // method can find the correct method when indicated type inherits the
  // method from its interface's default method
  inner class FinalLayer9 : ILayerNoOverride {
    fun getLayer(): String = super<ILayerNoOverride>.layer()
  }

  // Like FinalLayer9, but to test out if the rule for most specific default
  // method also applies for invoke-super on default interface method
  inner class FinalLayer10 : ILayerOverrideChild2 {
    fun getLayer(): String = super<ILayerOverrideChild2>.layer()
  }

  // Check if resolving invoke-interface on ILayer.layer()
  // and resolving invoke-virtual on FinalLayer11.Layer() will generate
  // different result.
  inner class FinalLayer11 : MidLayer5(), ILayer

  @Test
  fun testInvokeVirtualOnClass() {
    val simple = SimpleGreeter()
    // Hello from IGreeter
    assertThat(simple.greet()).isEqualTo("Hello from IGreeter")
    // Goodbye from IGreeter
    assertThat(simple.farewell()).isEqualTo("Goodbye from IGreeter")
    // Hello from IGreeter
    assertThat(simple.nestedgreet()).isEqualTo("Hello from IGreeter")
    // nonDefaultString from SimpleGreeter
    assertThat(simple.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter")
  }

  @Test
  fun testInvokeInterfaceOnInterface() {
    val simple2: IGreeter = SimpleGreeter()
    // Hello from IGreeter
    assertThat(simple2.greet()).isEqualTo("Hello from IGreeter")
    // Goodbye from IGreeter
    assertThat(simple2.farewell()).isEqualTo("Goodbye from IGreeter")
    // Hello from IGreeter
    assertThat(simple2.nestedgreet()).isEqualTo("Hello from IGreeter")
    // nonDefaultString from SimpleGreeter
    assertThat(simple2.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter")
  }

  @Test
  fun testInvokeVirtualOnOverriddenMethod() {
    val complex = ComplexGreeter()
    // Hello from ComplexGreeter
    assertThat(complex.greet()).isEqualTo("Hello from ComplexGreeter")
    // Goodbye from IGreeter
    assertThat(complex.farewell()).isEqualTo("Goodbye from IGreeter")
    // Need to pay attention to case like this during method analysis
    // Hello from ComplexGreeter
    assertThat(complex.nestedgreet()).isEqualTo("Hello from ComplexGreeter")
    // nonDefaultString from SimpleGreeter
    assertThat(complex.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter")
  }

  @Test
  fun testInvokeVirtualOnParentClass() {
    val complex2: SimpleGreeter = ComplexGreeter()
    // Hello from ComplexGreeter
    assertThat(complex2.greet()).isEqualTo("Hello from ComplexGreeter")
    // Goodbye from IGreeter
    assertThat(complex2.farewell()).isEqualTo("Goodbye from IGreeter")
    // Need to pay attention to case like this during method analysis
    // Hello from ComplexGreeter
    assertThat(complex2.nestedgreet()).isEqualTo("Hello from ComplexGreeter")
    // nonDefaultString from SimpleGreeter
    assertThat(complex2.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter")
  }

  @Test
  fun testInvokeInterfaceOnOverriddenMethod() {
    val complex2: IGreeter = ComplexGreeter()
    // Hello from ComplexGreeter
    assertThat(complex2.greet()).isEqualTo("Hello from ComplexGreeter")
    // Goodbye from IGreeter
    assertThat(complex2.farewell()).isEqualTo("Goodbye from IGreeter")
    // Need to pay attention to case like this during method analysis
    // Hello from ComplexGreeter
    assertThat(complex2.nestedgreet()).isEqualTo("Hello from ComplexGreeter")
    // nonDefaultString from SimpleGreeter
    assertThat(complex2.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter")
  }

  @Test
  fun testInvokeVirtualWithOverride() {
    val custom = CustomGreeter()
    // Hello from CustomGreeter
    assertThat(custom.greet()).isEqualTo("Hello from CustomGreeter")
    // Goodbye from IGreeter
    assertThat(custom.farewell()).isEqualTo("Goodbye from IGreeter")
    // Hello from CustomGreeter
    assertThat(custom.nestedgreet()).isEqualTo("Hello from CustomGreeter")
    // nonDefaultString from CustomGreeter
    assertThat(custom.nonDefaultString()).isEqualTo("nonDefaultString from CustomGreeter")
  }

  @Test
  fun testInvokeVirtualOnMultipleInterfaces() {
    val formal = DoubleCustomGreeter()
    // Hello from DoubleCustomGreeter
    assertThat(formal.greet()).isEqualTo("Hello from DoubleCustomGreeter")
    // Hello from DoubleCustomGreeter
    assertThat(formal.nestedgreet()).isEqualTo("Hello from DoubleCustomGreeter")
    // nonDefaultString from DoubleCustomGreeter
    assertThat(formal.nonDefaultString()).isEqualTo("nonDefaultString from DoubleCustomGreeter")
  }

  @Test
  fun testInvokeInterfaceOnMultipleInterfaces() {
    val formal: IGreeter = DoubleCustomGreeter()
    // Hello from DoubleCustomGreeter
    assertThat(formal.greet()).isEqualTo("Hello from DoubleCustomGreeter")
    // Hello from DoubleCustomGreeter
    assertThat(formal.nestedgreet()).isEqualTo("Hello from DoubleCustomGreeter")
    // nonDefaultString from DoubleCustomGreeter
    assertThat(formal.nonDefaultString()).isEqualTo("nonDefaultString from DoubleCustomGreeter")
  }

  @Test
  fun testInvokeInterfaceOnMultipleInterfaces2() {
    val formal: IFormalGreeter = DoubleCustomGreeter()
    // Hello from DoubleCustomGreeter
    assertThat(formal.greet()).isEqualTo("Hello from DoubleCustomGreeter")
  }

  @Test
  fun testInvokeSuperInterface() {
    val superCalling = SuperCallingGreeter()
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.greet())
        .isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter")
    // Hello from IGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.wrapGreet())
        .isEqualTo("Hello from IGreeter and ParentClass and SuperCallingGreeter")
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.nestedgreet())
        .isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter")
    // nonDefaultString from SuperCallingGreeter
    assertThat(superCalling.nonDefaultString())
        .isEqualTo("nonDefaultString from SuperCallingGreeter")
  }

  @Test
  fun testInvokeInterfaceInvokeSuperInterface() {
    val superCalling: IGreeter = SuperCallingGreeter()
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.greet())
        .isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter")
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.nestedgreet())
        .isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter")
    // nonDefaultString from SuperCallingGreeter
    assertThat(superCalling.nonDefaultString())
        .isEqualTo("nonDefaultString from SuperCallingGreeter")
  }

  @Test
  fun testInvokeInterfaceInvokeSuperInterface2() {
    val superCalling: IFormalGreeter = SuperCallingGreeter()
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.greet())
        .isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter")
  }

  @Test
  fun testInvokeStaticInterface() {
    // Static hello from IGreeter
    assertThat(IGreeter.staticGreet()).isEqualTo("Static hello from IGreeter")
  }

  @Test
  fun testInvokeVirtualOnConcreteClass() {
    val concrete = ConcreteGreeter()
    // Hello from ConcreteGreeter
    assertThat(concrete.greet()).isEqualTo("Hello from ConcreteGreeter")
    // Additional hello from ConcreteGreeter
    assertThat(concrete.additionalGreet()).isEqualTo("Additional hello from ConcreteGreeter")
    // Goodbye from ConcreteGreeter
    assertThat(concrete.farewell()).isEqualTo("Goodbye from ConcreteGreeter")
    // Hello from ConcreteGreeter
    assertThat(concrete.nestedgreet()).isEqualTo("Hello from ConcreteGreeter")
    // nonDefaultString from ConcreteGreeter
    assertThat(concrete.nonDefaultString()).isEqualTo("nonDefaultString from ConcreteGreeter")
  }

  @Test
  fun testInvokeVirtualOnAbstractClass() {
    val concrete: AbstractGreeter = ConcreteGreeter()
    // Hello from ConcreteGreeter
    assertThat(concrete.greet()).isEqualTo("Hello from ConcreteGreeter")
    // Additional hello from ConcreteGreeter
    assertThat(concrete.additionalGreet()).isEqualTo("Additional hello from ConcreteGreeter")
    // Goodbye from ConcreteGreeter
    assertThat(concrete.farewell()).isEqualTo("Goodbye from ConcreteGreeter")
    // othermsg from IGreeter
    assertThat(concrete.othermsg()).isEqualTo("othermsg from IGreeter")
    // Need to pay attention to case like this during method analysis
    // Hello from ConcreteGreeter
    assertThat(concrete.nestedgreet()).isEqualTo("Hello from ConcreteGreeter")
    // nonDefaultString from ConcreteGreeter
    assertThat(concrete.nonDefaultString()).isEqualTo("nonDefaultString from ConcreteGreeter")
  }

  @Test
  fun testInvokeInterfaceOnConcreteClass() {
    val concrete: IGreeter = ConcreteGreeter()
    // Hello from ConcreteGreeter
    assertThat(concrete.greet()).isEqualTo("Hello from ConcreteGreeter")
    // Goodbye from ConcreteGreeter
    assertThat(concrete.farewell()).isEqualTo("Goodbye from ConcreteGreeter")
    // othermsg from IGreeter
    assertThat(concrete.othermsg()).isEqualTo("othermsg from IGreeter")
    // Need to pay attention to case like this during method analysis
    // Hello from ConcreteGreeter
    assertThat(concrete.nestedgreet()).isEqualTo("Hello from ConcreteGreeter")
    // nonDefaultString from ConcreteGreeter
    assertThat(concrete.nonDefaultString()).isEqualTo("nonDefaultString from ConcreteGreeter")
  }

  @Test
  fun testInvokeInterfaceOnReferences() {
    val simple = SimpleGreeter()
    val custom = CustomGreeter()
    val complex = ComplexGreeter()

    var greeterRef: IGreeter = simple
    // Hello from IGreeter
    assertThat(greeterRef.greet()).isEqualTo("Hello from IGreeter")

    greeterRef = custom
    // Hello from CustomGreeter
    assertThat(greeterRef.greet()).isEqualTo("Hello from CustomGreeter")

    // Hello from ComplexGreeter
    greeterRef = complex
    assertThat(greeterRef.greet()).isEqualTo("Hello from ComplexGreeter")
  }

  @Test
  fun testLayeredInvokeMultipleInterfaces1() {
    val finalLayer = FinalLayer1()
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeMultipleInterfaces2() {
    val finalLayer = FinalLayer2()
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first")
    // first
    assertThat(finalLayer.layer()).isEqualTo("first")
  }

  @Test
  fun testLayeredInvokeMultipleInterfaces3() {
    val finalLayer = FinalLayer3()
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeClassAndInterface1() {
    val finalLayer = FinalLayer4()
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeClassAndInterface2() {
    val finalLayer = FinalLayer4_2()
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeClassAndInterface2_2() {
    val finalLayer: ILayer = FinalLayer4_2()
    // pay attention to method override graph
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeClassAndInterface3() {
    val finalLayer = FinalLayer4_3()
    // top
    assertThat(finalLayer.getLayer()).isEqualTo("top")
    // top
    assertThat(finalLayer.layer()).isEqualTo("top")
  }

  @Test
  fun testLayeredInvokeClassAndInterface3_2() {
    val finalLayer: ILayerConflict = FinalLayer4_3()
    // Pay attention to this in method override graph analysis
    // top
    assertThat(finalLayer.layer()).isEqualTo("top")
  }

  @Test
  fun testLayeredInvokeWithRedundantInterfaces() {
    val finalLayer = FinalLayer5()
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeWithClassMultipleInterface() {
    val finalLayer = FinalLayer6()
    // four
    assertThat(finalLayer.getLayer()).isEqualTo("four")
    // four
    assertThat(finalLayer.layer()).isEqualTo("four")
  }

  @Test
  fun testLayeredInvokeWithDiamondInterface1() {
    val finalLayer = FinalLayer7()
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeWithDiamondInterface2() {
    val finalLayer = FinalLayer8()
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeSuperWithInheritedDefault() {
    val finalLayer = FinalLayer9()
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first")
    // first
    assertThat(finalLayer.layer()).isEqualTo("first")
  }

  @Test
  fun testLayeredInvokeSuperWithInheritedDefault2() {
    val finalLayer = FinalLayer10()
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second")
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeSuperWithInheritedDefault2_2() {
    val finalLayer: ILayerOverrideChild2 = FinalLayer10()
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeSuperWithInheritedDefault2_3() {
    val finalLayer: ILayer = FinalLayer10()
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testLayeredInvokeSuperWithInheritedDefault2_4() {
    val finalLayer: ILayerOverride = FinalLayer10()
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testInvokeInterfaceVirtualDifference() {
    val finalLayer = FinalLayer11()
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }

  @Test
  fun testInvokeInterfaceVirtualDifference2() {
    val finalLayer: ILayer = FinalLayer11()
    // second
    assertThat(finalLayer.layer()).isEqualTo("second")
  }
}
