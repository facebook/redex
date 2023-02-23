/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import com.facebook.annotations.OkToExtend;
import com.facebook.redex.test.instr.KeepForRedexTest;
import org.junit.Test;

import static org.fest.assertions.api.Assertions.assertThat;

/**
 * This test mimics a seriously complicated inheritance hierachy where non-def miranda methods exist in the virtual scopes.
 * In addition, some mergeable leaf classes do not override a virtual method defined by a base class.
 * In this case, in the merged virtual dispatch switch, we need to fall back to the proper base method implementation.
 * Given that the top_def of the virtual scope can be a non-def miranda, identifying the correct base method to invoke in the
 * virtual dispatch switch, can be tricky. This test ensures that we encode the virtual dispatch correctly in the generated
 * shape class.
 */

interface Intf { String method(); }

// abstract base, does not implement any method, but has a Miranda method.
@OkToExtend
abstract class AbstractBase implements Intf {}

interface IntfD {
  default int foo() { return 42; }
}

@OkToExtend
abstract class NonAbstractBase {
  public int foo() { return 12; }
}

@KeepForRedexTest
public class ClassMergingMirandaTest {

  @OkToExtend
  class BaseA extends AbstractBase {
    public String method() { return "BaseA"; }
  }

  class SubA1 extends BaseA {
    public String method() { return "SubA1"; }
  }
  class SubA2 extends BaseA {}

  @KeepForRedexTest
  @Test
  public void testOneImpleBaseCls() {
    BaseA s1 = new SubA1();
    BaseA s2 = new SubA2();

    assertThat(s1.method()).isEqualTo("SubA1");
    assertThat(s2.method()).isEqualTo("BaseA");
  }

  @OkToExtend
  class BaseB1 extends AbstractBase {
    public String method() { return "BaseB1"; }
  }

  @OkToExtend
  class BaseB2 extends BaseB1 {
    public String method() { return "BaseB2"; }
  }

  class SubB1 extends BaseB2 {
    public String method() { return "SubB1"; }
  }
  class SubB2 extends BaseB2 {}

  @KeepForRedexTest
  @Test
  public void testTwoImplBaseCls() {
    BaseB1 s1 = new SubB1();
    BaseB1 s2 = new SubB2();

    assertThat(s1.method()).isEqualTo("SubB1");
    assertThat(s2.method()).isEqualTo("BaseB2");
  }

  @OkToExtend
  abstract class BaseC1 extends AbstractBase {}

  @OkToExtend
  class BaseC2 extends BaseC1 {
    public String method() { return "BaseC2"; }
  }

  class SubC1 extends BaseC2 {
    public String method() { return "SubC1"; }
  }
  class SubC2 extends BaseC2 {}

  @KeepForRedexTest
  @Test
  public void testTwoAbstractCls() {
    BaseC1 s1 = new SubC1();
    BaseC1 s2 = new SubC2();

    assertThat(s1.method()).isEqualTo("SubC1");
    assertThat(s2.method()).isEqualTo("BaseC2");
  }

  @OkToExtend
  class BaseD1 extends AbstractBase {
    public String method() { return "BaseD1"; }
  }

  @OkToExtend
  class BaseD2 extends BaseD1 {}

  @OkToExtend
  class BaseD3 extends BaseD2 {
    public String method() { return "BaseD3"; }
  }

  class SubD1 extends BaseD3 {
    public String method() { return "SubD1"; }
  }
  class SubD2 extends BaseD3 {}

  @KeepForRedexTest
  @Test
  public void testTwoAbstractTwoImpls() {
    BaseD1 s1 = new SubD1();
    BaseD1 s2 = new SubD2();

    assertThat(s1.method()).isEqualTo("SubD1");
    assertThat(s2.method()).isEqualTo("BaseD3");
  }

  @OkToExtend
  class SubE1 extends NonAbstractBase implements IntfD {
    @Override
    public int foo() { return 22; }
  }

  @OkToExtend
  class SubE2 extends NonAbstractBase implements IntfD {}

  @KeepForRedexTest
  @Test
  public void testBaseImplWithDefaultMethod() {
    NonAbstractBase s1 = new SubE1();
    NonAbstractBase s2 = new SubE2();

    assertThat(s1.foo()).isEqualTo(22);
    assertThat(s2.foo()).isEqualTo(12);

    IntfD id1 = new SubE1();
    IntfD id2 = new SubE2();
    assertThat(id1.foo()).isEqualTo(22);
    assertThat(id2.foo()).isEqualTo(12);
  }
}
