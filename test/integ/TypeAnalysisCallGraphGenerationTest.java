/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

public class TypeAnalysisCallGraphGenerationTest {
  static void main() {
    Base b = new Base();
    b.getVal();
  }
}
