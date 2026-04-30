/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used to test default interface methods (Java 8 feature).
 * It tests various scenarios including:
 * - Default interface methods
 * - Static interface methods
 * - Class implementing interface with default method
 * - Class overriding default method
 * - Multiple interfaces with default methods
 */

package com.facebook.redextest;

import static org.assertj.core.api.Assertions.assertThat;
import org.junit.Test;


public class Dex037 {

  // Interface with default methods
  interface IGreeter {
    private String emptyString() { return ""; }

    default String greet() { return emptyString() + "Hello from IGreeter"; }

    default String farewell() { return "Goodbye from IGreeter"; }

    default String nestedgreet() { return greet(); }

    default String othermsg() { return "othermsg from IGreeter"; }

    String nonDefaultString();

    static String staticGreet() { return "Static hello from IGreeter"; }
  }

  // Interface with another default method
  interface IFormalGreeter {
    default String greet() { return "Hello from IFormalGreeter"; }
  }

  // Class implementing interface without overriding default method
  class SimpleGreeter implements IGreeter {
    // Uses default implementations from Greeter except nonDefaultString
    // that need implementation
    public String nonDefaultString() {
      return "nonDefaultString from SimpleGreeter";
    }
  }

  // Class extending class implementing interface overriding default method
  class ComplexGreeter extends SimpleGreeter {
    @Override
    public String greet() {
      return "Hello from ComplexGreeter";
    }
  }

  // Class implementing interface and overriding default method
  class CustomGreeter implements IGreeter {
    @Override
    public String greet() {
      return "Hello from CustomGreeter";
    }

    public String nonDefaultString() {
      return "nonDefaultString from CustomGreeter";
    }
    // farewell() still uses default implementation
  }

  // Class implementing multiple interfaces with same default method signature
  class DoubleCustomGreeter implements IGreeter, IFormalGreeter {
    // Must override greet() to resolve conflict
    @Override
    public String greet() {
      return "Hello from DoubleCustomGreeter";
    }

    public String nonDefaultString() {
      return "nonDefaultString from DoubleCustomGreeter";
    }
  }

  class Parent {
    public String foo() { return "ParentClass"; }
  }

  // Class calling super interface default method
  class SuperCallingGreeter
      extends Parent implements IGreeter, IFormalGreeter {
    @Override
    public String greet() {
      return IFormalGreeter.super.greet() + " and " + super.foo() +
          " and SuperCallingGreeter";
    }

    public String wrapGreet() {
      return IGreeter.super.greet() + " and " + super.foo() +
          " and SuperCallingGreeter";
    }

    public String nonDefaultString() {
      return "nonDefaultString from SuperCallingGreeter";
    }
  }

  // Abstract class implementing interface
  abstract class AbstractGreeter implements IGreeter {
    public abstract String additionalGreet();
    public abstract String greet();
  }

  // Concrete class extending abstract class
  class ConcreteGreeter extends AbstractGreeter {
    public String greet() { return "Hello from ConcreteGreeter"; }

    @Override
    public String additionalGreet() {
      return "Additional hello from ConcreteGreeter";
    }

    @Override
    public String farewell() {
      return "Goodbye from ConcreteGreeter";
    }

    public String nonDefaultString() {
      return "nonDefaultString from ConcreteGreeter";
    }
  }

  interface ILayer {
    default String layer() { return "first"; }
  }

  interface ILayerOverride extends ILayer {
    default String layer() { return "second"; }
  }

  interface ILayerOverrideChild extends ILayerOverride {}

  interface ILayerOverrideChild2 extends ILayer, ILayerOverride {}

  interface ILayerNoOverride extends ILayer {}

  // An interface to create default interface conflict
  interface ILayerConflict {
    default String layer() { return "third"; }
  }

  // Testing which layer will be resolved to, does it depend on interface
  // sequence in FinalLayer7 and FinalLayer8
  interface ILayerLast1 extends ILayerOverride, ILayerNoOverride {}
  interface ILayerLast2 extends ILayerNoOverride, ILayerOverride {}

  // The following three is testing resolver can find the
  // correct method regardless of the order of interfaces,
  // with and without the override in second layer of interface
  class Layer1 implements ILayerOverride, ILayer {}
  class Layer2 implements ILayerNoOverride, ILayer {}
  class Layer3 implements ILayer, ILayerOverride {}
  class FinalLayer1 extends Layer1 {
    public String getLayer() { return super.layer(); }
  }
  class FinalLayer2 extends Layer2 {
    public String getLayer() { return super.layer(); }
  }
  class FinalLayer3 extends Layer3 {
    public String getLayer() { return super.layer(); }
  }

  class MidLayer implements ILayer {}
  class MidLayer2
      implements ILayerNoOverride, ILayer, ILayerLast1, ILayerOverride {}
  class MidLayer3 implements ILayer {
    public String layer() { return "four"; }
  }

  class TopLayer {
    public String layer() { return "top"; }
  }
  class MidLayer4 extends TopLayer implements ILayer {}
  class MidLayer5 implements ILayerOverride {}

  // Test which method get resolved to when a class has a super class getting
  // layer method from its interface's default method, and there is conflicting
  // method in its interface
  class FinalLayer4 extends MidLayer implements ILayerOverride {
    public String getLayer() { return super.layer(); }
  }
  // Similar to above, but what if the direct super class and interface all
  // don't have a method and they all get it from their interface's default
  // method
  class FinalLayer4_2 extends MidLayer implements ILayerOverrideChild {
    public String getLayer() { return super.layer(); }
  }

  // Similar to above, but what if the direct super class don't have a method,
  // super class inherit from its super class, interface do have a direct
  // method
  class FinalLayer4_3 extends MidLayer4 implements ILayerConflict {
    public String getLayer() { return super.layer(); }
  }

  // Test which method get resolved to when a class has a super class
  // that has a complicated interface hierachy
  class FinalLayer5 extends MidLayer2 {
    public String getLayer() { return super.layer(); }
  }

  // Test which method get resolved to when a class has a super class has
  // layer method, and its two interfaces have conflicting layer methods
  class FinalLayer6 extends MidLayer3 implements ILayer, ILayerConflict {
    public String getLayer() { return super.layer(); }
  }

  // Testing which layer will be resolved to, does it depend on interface
  // sequence
  class FinalLayer7 implements ILayerLast1 {
    public String getLayer() { return layer(); }
  }
  class FinalLayer8 implements ILayerLast2 {
    public String getLayer() { return layer(); }
  }

  // This is to check if resolving invoke-super for default interface
  // method can find the correct method when indicated type inherits the
  // method from its interface's default method
  class FinalLayer9 implements ILayerNoOverride {
    public String getLayer() { return ILayerNoOverride.super.layer(); }
  }

  // Like FinalLayer9, but to test out if the rule for most specific default
  // method also applies for invoke-super on default interface method
  class FinalLayer10 implements ILayerOverrideChild2 {
    public String getLayer() { return ILayerOverrideChild2.super.layer(); }
  }

  // Check if resolving invoke-interface on ILayer.layer()
  // and resolving invoke-virtual on FinalLayer11.Layer() will generate
  // different result.
  class FinalLayer11 extends MidLayer5 implements ILayer {}
  
  @Test
  public void testInvokeVirtualOnClass() {
    SimpleGreeter simple = new SimpleGreeter();
    // Hello from IGreeter
    assertThat(simple.greet()).isEqualTo("Hello from IGreeter");
    // Goodbye from IGreeter
    assertThat(simple.farewell()).isEqualTo("Goodbye from IGreeter");
    // Hello from IGreeter
    assertThat(simple.nestedgreet()).isEqualTo("Hello from IGreeter");
    // nonDefaultString from SimpleGreeter
    assertThat(simple.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter");
  }
  
  @Test
  public void testInvokeInterfaceOnInterface() {
    IGreeter simple2 = new SimpleGreeter();
    // Hello from IGreeter
    assertThat(simple2.greet()).isEqualTo("Hello from IGreeter");
    // Goodbye from IGreeter
    assertThat(simple2.farewell()).isEqualTo("Goodbye from IGreeter");
    // Hello from IGreeter
    assertThat(simple2.nestedgreet()).isEqualTo("Hello from IGreeter");
    // nonDefaultString from SimpleGreeter
    assertThat(simple2.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter");
  }
  
  @Test
  public void testInvokeVirtualOnOverriddenMethod() {
    ComplexGreeter complex = new ComplexGreeter();
    // Hello from ComplexGreeter
    assertThat(complex.greet()).isEqualTo("Hello from ComplexGreeter");
    // Goodbye from IGreeter
    assertThat(complex.farewell()).isEqualTo("Goodbye from IGreeter");
    // Need to pay attention to case like this during method analysis
    // Hello from ComplexGreeter
    assertThat(complex.nestedgreet()).isEqualTo("Hello from ComplexGreeter");
    // nonDefaultString from SimpleGreeter
    assertThat(complex.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter");
  }
  
  @Test
  public void testInvokeVirtualOnParentClass() {
    SimpleGreeter complex2 = new ComplexGreeter();
    // Hello from ComplexGreeter
    assertThat(complex2.greet()).isEqualTo("Hello from ComplexGreeter");
    // Goodbye from IGreeter
    assertThat(complex2.farewell()).isEqualTo("Goodbye from IGreeter");
    // Need to pay attention to case like this during method analysis
    // Hello from ComplexGreeter
    assertThat(complex2.nestedgreet()).isEqualTo("Hello from ComplexGreeter");
    // nonDefaultString from SimpleGreeter
    assertThat(complex2.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter");
  }
  
  @Test
  public void testInvokeInterfaceOnOverriddenMethod() {
    IGreeter complex2 = new ComplexGreeter();
    // Hello from ComplexGreeter
    assertThat(complex2.greet()).isEqualTo("Hello from ComplexGreeter");
    // Goodbye from IGreeter
    assertThat(complex2.farewell()).isEqualTo("Goodbye from IGreeter");
    // Need to pay attention to case like this during method analysis
    // Hello from ComplexGreeter
    assertThat(complex2.nestedgreet()).isEqualTo("Hello from ComplexGreeter");
    // nonDefaultString from SimpleGreeter
    assertThat(complex2.nonDefaultString()).isEqualTo("nonDefaultString from SimpleGreeter");
  }
  
  @Test
  public void testInvokeVirtualWithOverride() {
    CustomGreeter custom = new CustomGreeter();
    // Hello from CustomGreeter
    assertThat(custom.greet()).isEqualTo("Hello from CustomGreeter");
    // Goodbye from IGreeter
    assertThat(custom.farewell()).isEqualTo("Goodbye from IGreeter");
    // Hello from CustomGreeter
    assertThat(custom.nestedgreet()).isEqualTo("Hello from CustomGreeter");
    // nonDefaultString from CustomGreeter
    assertThat(custom.nonDefaultString()).isEqualTo("nonDefaultString from CustomGreeter");
  }
  
  @Test
  public void testInvokeVirtualOnMultipleInterfaces() {
    DoubleCustomGreeter formal = new DoubleCustomGreeter();
    // Hello from DoubleCustomGreeter
    assertThat(formal.greet()).isEqualTo("Hello from DoubleCustomGreeter");
    // Hello from DoubleCustomGreeter
    assertThat(formal.nestedgreet()).isEqualTo("Hello from DoubleCustomGreeter");
    // nonDefaultString from DoubleCustomGreeter
    assertThat(formal.nonDefaultString()).isEqualTo("nonDefaultString from DoubleCustomGreeter");
  }
  
  @Test
  public void testInvokeInterfaceOnMultipleInterfaces() {
    IGreeter formal = new DoubleCustomGreeter();
    // Hello from DoubleCustomGreeter
    assertThat(formal.greet()).isEqualTo("Hello from DoubleCustomGreeter");
    // Hello from DoubleCustomGreeter
    assertThat(formal.nestedgreet()).isEqualTo("Hello from DoubleCustomGreeter");
    // nonDefaultString from DoubleCustomGreeter
    assertThat(formal.nonDefaultString()).isEqualTo("nonDefaultString from DoubleCustomGreeter");
  }
  
  @Test
  public void testInvokeInterfaceOnMultipleInterfaces2() {
    IFormalGreeter formal = new DoubleCustomGreeter();
    // Hello from DoubleCustomGreeter
    assertThat(formal.greet()).isEqualTo("Hello from DoubleCustomGreeter");
  }
  
  @Test
  public void testInvokeSuperInterface() {
    SuperCallingGreeter superCalling = new SuperCallingGreeter();
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.greet()).isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter");
    // Hello from IGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.wrapGreet()).isEqualTo("Hello from IGreeter and ParentClass and SuperCallingGreeter");
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.nestedgreet()).isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter");
    // nonDefaultString from SuperCallingGreeter
    assertThat(superCalling.nonDefaultString()).isEqualTo("nonDefaultString from SuperCallingGreeter");
  }
  
  @Test
  public void testInvokeInterfaceInvokeSuperInterface() {
    IGreeter superCalling = new SuperCallingGreeter();
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.greet()).isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter");
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.nestedgreet()).isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter");
    // nonDefaultString from SuperCallingGreeter
    assertThat(superCalling.nonDefaultString()).isEqualTo("nonDefaultString from SuperCallingGreeter");
  }
  
  @Test
  public void testInvokeInterfaceInvokeSuperInterface2() {
    IFormalGreeter superCalling = new SuperCallingGreeter();
    // Hello from IFormalGreeter and ParentClass and SuperCallingGreeter
    assertThat(superCalling.greet()).isEqualTo("Hello from IFormalGreeter and ParentClass and SuperCallingGreeter");
  }
  
  @Test
  public void testInvokeStaticInterface() {
    // Static hello from IGreeter
    assertThat(IGreeter.staticGreet()).isEqualTo("Static hello from IGreeter");
  }
  
  @Test
  public void testInvokeVirtualOnConcreteClass() {
    ConcreteGreeter concrete = new ConcreteGreeter();
    // Hello from ConcreteGreeter
    assertThat(concrete.greet()).isEqualTo("Hello from ConcreteGreeter");
    // Additional hello from ConcreteGreeter
    assertThat(concrete.additionalGreet()).isEqualTo("Additional hello from ConcreteGreeter");
    // Goodbye from ConcreteGreeter
    assertThat(concrete.farewell()).isEqualTo("Goodbye from ConcreteGreeter");
    // Hello from ConcreteGreeter
    assertThat(concrete.nestedgreet()).isEqualTo("Hello from ConcreteGreeter");
    // nonDefaultString from ConcreteGreeter
    assertThat(concrete.nonDefaultString()).isEqualTo("nonDefaultString from ConcreteGreeter");
  }
  
  @Test
  public void testInvokeVirtualOnAbstractClass() {
    AbstractGreeter concrete = new ConcreteGreeter();
    // Hello from ConcreteGreeter
    assertThat(concrete.greet()).isEqualTo("Hello from ConcreteGreeter");
    // Additional hello from ConcreteGreeter
    assertThat(concrete.additionalGreet()).isEqualTo("Additional hello from ConcreteGreeter");
    // Goodbye from ConcreteGreeter
    assertThat(concrete.farewell()).isEqualTo("Goodbye from ConcreteGreeter");
    // othermsg from IGreeter
    assertThat(concrete.othermsg()).isEqualTo("othermsg from IGreeter");
    // Need to pay attention to case like this during method analysis
    // Hello from ConcreteGreeter
    assertThat(concrete.nestedgreet()).isEqualTo("Hello from ConcreteGreeter");
    // nonDefaultString from ConcreteGreeter
    assertThat(concrete.nonDefaultString()).isEqualTo("nonDefaultString from ConcreteGreeter");
  }
  
  @Test
  public void testInvokeInterfaceOnConcreteClass() {
    IGreeter concrete = new ConcreteGreeter();
    // Hello from ConcreteGreeter
    assertThat(concrete.greet()).isEqualTo("Hello from ConcreteGreeter");
    // Goodbye from ConcreteGreeter
    assertThat(concrete.farewell()).isEqualTo("Goodbye from ConcreteGreeter");
    // othermsg from IGreeter
    assertThat(concrete.othermsg()).isEqualTo("othermsg from IGreeter");
    // Need to pay attention to case like this during method analysis
    // Hello from ConcreteGreeter
    assertThat(concrete.nestedgreet()).isEqualTo("Hello from ConcreteGreeter");
    // nonDefaultString from ConcreteGreeter
    assertThat(concrete.nonDefaultString()).isEqualTo("nonDefaultString from ConcreteGreeter");
  }
  
  @Test
  public void testInvokeInterfaceOnReferences() {
    SimpleGreeter simple = new SimpleGreeter();
    CustomGreeter custom = new CustomGreeter();
    ComplexGreeter complex = new ComplexGreeter();

    IGreeter greeterRef = simple;
    // Hello from IGreeter
    assertThat(greeterRef.greet()).isEqualTo("Hello from IGreeter");

    greeterRef = custom;
    // Hello from CustomGreeter
    assertThat(greeterRef.greet()).isEqualTo("Hello from CustomGreeter");

    // Hello from ComplexGreeter
    greeterRef = complex;
    assertThat(greeterRef.greet()).isEqualTo("Hello from ComplexGreeter");
  }
  
  @Test
  public void testLayeredInvokeMultipleInterfaces1() {
    FinalLayer1 finalLayer = new FinalLayer1();
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeMultipleInterfaces2() {
    FinalLayer2 finalLayer = new FinalLayer2();
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first");
    // first
    assertThat(finalLayer.layer()).isEqualTo("first");
  }
  
  @Test
  public void testLayeredInvokeMultipleInterfaces3() {
    FinalLayer3 finalLayer = new FinalLayer3();
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeClassAndInterface1() {
    FinalLayer4 finalLayer = new FinalLayer4();
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeClassAndInterface2() {
    FinalLayer4_2 finalLayer = new FinalLayer4_2();
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeClassAndInterface2_2() {
    ILayer finalLayer = new FinalLayer4_2();
    // pay attention to method override graph
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeClassAndInterface3() {
    FinalLayer4_3 finalLayer = new FinalLayer4_3();
    // top
    assertThat(finalLayer.getLayer()).isEqualTo("top");
    // top
    assertThat(finalLayer.layer()).isEqualTo("top");
  }
  
  @Test
  public void testLayeredInvokeClassAndInterface3_2() {
    ILayerConflict finalLayer = new FinalLayer4_3();
    // Pay attention to this in method override graph analysis
    // top
    assertThat(finalLayer.layer()).isEqualTo("top");
  }
  
  @Test
  public void testLayeredInvokeWithRedundantInterfaces() {
    FinalLayer5 finalLayer = new FinalLayer5();
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeWithClassMultipleInterface() {
    FinalLayer6 finalLayer = new FinalLayer6();
    // four
    assertThat(finalLayer.getLayer()).isEqualTo("four");
    // four
    assertThat(finalLayer.layer()).isEqualTo("four");
  }
  
  @Test
  public void testLayeredInvokeWithDiamondInterface1() {
    FinalLayer7 finalLayer = new FinalLayer7();
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeWithDiamondInterface2() {
    FinalLayer8 finalLayer = new FinalLayer8();
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeSuperWithInheritedDefault() {
    FinalLayer9 finalLayer = new FinalLayer9();
    // first
    assertThat(finalLayer.getLayer()).isEqualTo("first");
    // first
    assertThat(finalLayer.layer()).isEqualTo("first");
  }
  
  @Test
  public void testLayeredInvokeSuperWithInheritedDefault2() {
    FinalLayer10 finalLayer = new FinalLayer10();
    // second
    assertThat(finalLayer.getLayer()).isEqualTo("second");
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeSuperWithInheritedDefault2_2() {
    ILayerOverrideChild2 finalLayer = new FinalLayer10();
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeSuperWithInheritedDefault2_3() {
    ILayer finalLayer = new FinalLayer10();
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testLayeredInvokeSuperWithInheritedDefault2_4() {
    ILayerOverride finalLayer = new FinalLayer10();
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
  
  @Test
  public void testInvokeInterfaceVirtualDifference() {
    FinalLayer11 finalLayer = new FinalLayer11();
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }

  @Test
  public void testInvokeInterfaceVirtualDifference2() {
    ILayer finalLayer = new FinalLayer11();
    // second
    assertThat(finalLayer.layer()).isEqualTo("second");
  }
}
