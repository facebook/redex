/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class InitReadsNothing {
  static {

  }
}

class InitDirectlyReadsOneStaticField {
  static int field1 = 1;
  static int field2 = 2;
  static {
    field2 = field1;
  }
}

class InitIndirectlyReadsOneStaticField {
  static int field1 = 1;
  static int field2 = 2;
  static {
    initHelper();
  }

  static void initHelper() {
    field2 = field1;
  }
}

class InitInvokesRecursion {
  static int field1 = 10;
  static {
    initHelper();
  }

  static void initHelper() {
    if (field1 == 0) { return; }
    field1 = field1 - 1;
    initHelper();
  }
}

class InitInvokesMutualRecursion {
  static int field1 = 5;
  static int field2 = 6;
  static {
    initHelper1();
  }

  static void initHelper1() {
    if (field1 == 0) { return; }
    field2 = field1 - 1;
    initHelper2();
  }

  static void initHelper2() {
    field1 = field2;
    initHelper1();
  }
}

class InitHelper1 {
  InitHelper1() {}
  void help() {
    InitInvokesVirtual.field2 = InitInvokesVirtual.field1;
  }
}

class InitInvokesVirtual {
  static int field1 = 10;
  static int field2 = 5;
  static {
    InitHelper1 helper = new InitHelper1();
    helper.help();
  }
}

class InitHelper2 {
  InitHelper2() {}

  void help() {
    if (InitInvokesVirtualRecursion.field1 > 0) {
      return;
    }
    InitInvokesVirtualRecursion.field2 = InitInvokesVirtualRecursion.field1 - 1;
    InitInvokesVirtualRecursion.field1 = InitInvokesVirtualRecursion.field2;
    help();
  }
}

class InitInvokesVirtualRecursion {
  static int field1 = 5;
  static int field2 = 1;
  static {
    InitHelper2 helper = new InitHelper2();
    helper.help();
  }
}
