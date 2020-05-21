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
}

@OkToExtend
class SubOne extends Base {
  @Override
  String foo() {
    return "SubOne";
  }
}

@OkToExtend
class SubTwo extends Base {
  @Override
  String foo() {
    return "SubTwo";
  }
}

@KeepForRedexTest
public class TypeAnalysisTest {

  Base mBase;
  Base mNull;
  Base mNotNull;

  void setBase(Base b) { mBase = b; }

  Base getBase() { return mBase; }

  void setNull() { mNull = null; }

  Base getNull() { return mNull; }

  void setNotNull() { mNotNull = new SubTwo(); }

  Base getNotNull() { return mNotNull; }

  @KeepForRedexTest
  @Test
  public void testSetAndGet() {
    setBase(null);
    setBase(new SubOne());
    assertThat(getBase().foo()).isEqualTo("SubOne");
    setNull();
    assertThat(getNull()).isNull();
    setNotNull();
    assertThat(getNotNull().foo()).isEqualTo("SubTwo");
  }
}
