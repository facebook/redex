/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.assertj.core.api.Assertions.assertThat;

import com.facebook.annotations.OkToExtend;
import com.facebook.redex.test.instr.KeepForRedexTest;
import org.junit.Test;

@OkToExtend
class Base {
  String mStr;
  String foo() { return "Base"; }
  Base() { mStr = "Base"; }
}

@OkToExtend
class SubOne extends Base {
  SubOne() { super(); }

  @Override
  String foo() { return "SubOne"; }
}

@OkToExtend
class SubTwo extends Base {
  SubTwo() { super(); }

  @Override
  String foo() { return "SubTwo"; }
}

@KeepForRedexTest
public class TypeAnalysisAssertsTest {
  static Base sInstance;

  Base getInstance() {
    if (sInstance == null) {
      sInstance = new Base();
    }
    return sInstance;
  }

  Base mBase;
  Base mNull;
  Base mNotNull;
  Base mNullable;

  @KeepForRedexTest
  public TypeAnalysisAssertsTest() {}

  void setBase(Base b) { mBase = b; }

  Base getBase() { return mBase; }

  void setNull() { mNull = null; }

  Base getNull() { return mNull; }

  void setNotNull() { mNotNull = new SubTwo(); }

  Base getNotNull() { return mNotNull; }

  void initNullable() {
    if (mNullable == null) {
      mNullable = new SubTwo();
    }
  }

  Base getNullable() {
    return mNullable;
  }

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
    initNullable();
    assertThat(getNullable().foo()).isEqualTo("SubTwo");
  }

  @KeepForRedexTest
  @Test
  public void testSingleton() {
    assertThat(getInstance().foo()).isEqualTo("Base");
  }
}
