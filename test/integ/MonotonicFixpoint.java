/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class MonotonicFixpoint {
  /**
   * CFG:
   * B0 succs: B1 preds:
   * B1 succs: B1 B2 preds: B0 B1
   * B2 succs: preds: B1
   *   Block B0: --- live in: {}  live out: {v0, v2}
   *     [0x7f71bc002820] OPCODE: IOPCODE_LOAD_PARAM_OBJECT v4
   *     [0x7f71bc002c20] DEBUG: DBG_SET_PROLOGUE_END
   *     [0x7f71bc002c50] POSITION: MonotonicFixpoint.java:45
   *     [0x7f71bc0025c0] OPCODE: CONST v0
   *     [0x7f71bc002c80] DEBUG: DBG_START_LOCAL v0 a:I
   *     [0x7f71bc0025f0] OPCODE: CONST v1
   *     [0x7f71bc002cb0] DEBUG: DBG_START_LOCAL v1 b:I
   *     [0x7f71bc002670] OPCODE: CONST v2
   *   Block B1: --- live in: {v0, v2}  live out: {v0, v2}
   *     [0x7f71bc002bf0] TARGET: SIMPLE 0x7f71bc002a50
   *     [0x7f71bc002ce0] POSITION: MonotonicFixpoint.java:48 --- live in/out: {v0, v2}
   *     [0x7f71bc002d10] DEBUG: DBG_START_LOCAL v2 c:I
   *     [0x7f71bc0026f0] OPCODE: ADD_INT_LIT8 v1, v0
   *     [0x7f71bc002d40] POSITION: MonotonicFixpoint.java:49 --- live in/out: {v1, v2}
   *     [0x7f71bc002850] OPCODE: ADD_INT v2, v2, v1
   *     [0x7f71bc002d70] POSITION: MonotonicFixpoint.java:50 --- live in/out: {v1, v2}
   *     [0x7f71bc0028f0] OPCODE: MUL_INT_LIT8 v0, v1
   *     [0x7f71bc002da0] POSITION: MonotonicFixpoint.java:51 --- live in/out: {v0, v2}
   *     [0x7f71bc0029b0] OPCODE: CONST v3
   *     [0x7f71bc002a50] OPCODE: IF_LT v0, v3
   *   Block B2: --- live in: {v2}  live out: {}
   *     [0x7f71bc002dd0] POSITION: MonotonicFixpoint.java:52
   *     [0x7f71bc002b10] OPCODE: RETURN v2
   *
   */
  public int function_1() {
    int a = 0, b = 0, c = 0;
    // live in: {c}  live out: {a, c}
    do {
      b = a + 1; // live in: {a, c}  live out: {b, c}
      c = c + b; // live in: {b, c}  live out: {b, c}
      a = b * 2; // live in: {b, c}  live out: {a, c}
    } while (a < 9); // live in: {a, c}  live out: {a, c}
    return c; // live in: {c}  live out: {}
  }
}
