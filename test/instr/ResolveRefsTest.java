/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.fest.assertions.api.Assertions.assertThat;

import com.facebook.annotations.OkToExtend;
import com.facebook.redex.test.instr.KeepForRedexTest;
import org.junit.Test;

@OkToExtend
class Base {
  String foo() { return "Base"; }
  static Base getInstance() { return new Base(); }
}

@OkToExtend
class SubOne extends Base {
  static Base getInstance() { return new SubOne(); }
}

@OkToExtend
class SubTwo extends SubOne {
  @Override
  String foo() { return "SubTwo"; }
  static Base getInstance() { return new SubTwo(); }
}

class SubThree extends SubTwo {
  @Override
  String foo() { return "SubThree"; }
  static Base getInstance() { return new SubThree(); }
}

@KeepForRedexTest
public class ResolveRefsTest {

  @KeepForRedexTest
  @Test
  public void testSimpleInvokeVirtual() {
    Base b = new Base();
    assertThat(b.foo()).isEqualTo("Base");
    SubOne s1 = new SubOne();
    assertThat(s1.foo()).isEqualTo("Base");
    SubTwo s2 = new SubTwo();
    assertThat(s2.foo()).isEqualTo("SubTwo");
    SubThree s3 = new SubThree();
    assertThat(s3.foo()).isEqualTo("SubThree");
  }

  @KeepForRedexTest
  @Test
  public void testFactoryBaseInvokeVirtual() {
    Base b = Base.getInstance();
    assertThat(b.foo()).isEqualTo("Base");
    b = SubOne.getInstance();
    assertThat(b.foo()).isEqualTo("Base");
    b = SubTwo.getInstance();
    assertThat(b.foo()).isEqualTo("SubTwo");
    b = SubThree.getInstance();
    assertThat(b.foo()).isEqualTo("SubThree");
  }

  @KeepForRedexTest
  @Test
  public void testFactoryCastInvokeVirtual() {
    SubOne s1 = (SubOne) SubOne.getInstance();
    assertThat(s1.foo()).isEqualTo("Base");
    SubTwo s2 = (SubTwo) SubTwo.getInstance();
    assertThat(s2.foo()).isEqualTo("SubTwo");
    SubTwo s3 = (SubThree) SubThree.getInstance();
    assertThat(s3.foo()).isEqualTo("SubThree");
  }
}
