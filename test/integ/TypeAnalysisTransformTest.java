/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class Base {
  int getVal() { return 0; }
}

class SubOne extends Base {
  @Override
  int getVal() {
    return 1;
  }
}

class SubTwo extends Base {
  @Override
  int getVal() {
    return 2;
  }
}

/*
 * Anonymous class call back example
 */
interface Callback {
  void hook();
}

class RenderView {
  Callback mCallback = new Callback() {
    @Override
    public void hook() {
      checkBaseField();
    }
  };
  Base mBase;

  RenderView() { mBase = new Base(); }

  int checkBaseField() {
    if (mBase != null) {
      return 2;
    }
    return 1;
  }
}

/*
 * Primitive arrays
 */
class ReactNode {
  final boolean[] mIsCool = new boolean[4];

  void setCool(final int i, final boolean isCool) { mIsCool[i] = isCool; }

  String getCool(final int i) {
    if (mIsCool[i]) {
      return "cool";
    }
    return "not cool";
  }
}

class TestRemoveRedundantNullChecks {

  Base mField1;

  TestRemoveRedundantNullChecks() {
    int v = checkEQZInitReachable(mField1);
    v += checkEQZInitReachableGetField();
    mField1 = new Base();
  }

  public Base getSubOne() { return new SubOne(); }

  public Base getSubTwo() { return new SubTwo(); }

  public int checkEQZNullArg(Base b) {
    int i = 0;
    if (b != null) {
      i += b.getVal();
    }
    return i;
  }

  public int checkEQZNotNullArg(Base b) {
    int i = 0;
    if (b != null) {
      i += b.getVal();
    }
    return i;
  }

  public int checkNEZNullArg(Base b) {
    int i = 0;
    if (b == null) {
      i += b.getVal();
    }
    return i;
  }

  public int checkNEZNotNullArg(Base b) {
    int i = 0;
    if (b == null) {
      i += b.getVal();
    }
    return i;
  }

  private int checkEQZInitReachable(Base b) {
    int i = 0;
    if (b != null) {
      i += b.getVal();
    }
    return i;
  }

  private int checkEQZInitReachableGetField() {
    int i = 0;
    if (this.mField1 != null) {
      i += this.mField1.getVal();
    }
    return i;
  }

  static void main() {
    TestRemoveRedundantNullChecks t = new TestRemoveRedundantNullChecks();
    Base b = null;
    t.checkEQZNullArg(b);
    b = new Base();
    t.checkEQZNotNullArg(b);

    b = null;
    t.checkNEZNullArg(b);
    b = new Base();
    t.checkNEZNotNullArg(b);

    RenderView rv = new RenderView();
    rv.checkBaseField();

    ReactNode node = new ReactNode();
    node.setCool(0, true);
    node.setCool(1, false);
    String isCool = node.getCool(1);
  }
}

class TestRemoveRedundantTypeChecks {
  Base mField1;

  TestRemoveRedundantTypeChecks() {
    mField1 = new SubOne();
  }

  public int checkInstanceOfBaseNullArg(Base b) {
    int i = 42;
    if (b instanceof Base) {
      i += b.getVal();
    }
    return i;
  }

  public int checkInstanceOfBaseNotNullArg(Base b) {
    int i = 42;
    if (b instanceof Base) {
      i += b.getVal();
    }
    return i;
  }

  public int checkInstanceOfSubOneArg(Base b) {
    int i = 42;
    if (b instanceof SubOne) {
      i += ((SubOne) b).getVal();
    }
    return i;
  }

  public int checkInstanceOfSubTwoArg(Base b) {
    int i = 42;
    if (b instanceof SubTwo) {
      i += ((SubTwo) b).getVal();
    }
    return i;
  }

  public int checkInstanceOfNullableField() {
    int i = 42;
    Base b = mField1;
    if (b instanceof SubTwo) {
      i += ((SubTwo) b).getVal();
    }
    return i;
  }

  static void main() {
    TestRemoveRedundantTypeChecks t = new TestRemoveRedundantTypeChecks();
    Base b = null;
    t.checkInstanceOfBaseNullArg(b);
    b = new Base();
    t.checkInstanceOfBaseNotNullArg(b);

    b = new SubOne();
    t.checkInstanceOfSubOneArg(b);
    t.checkInstanceOfSubTwoArg(b);

    // null assignment making the field nullable
    t.mField1 = null;
    t.checkInstanceOfNullableField();
  }
}
