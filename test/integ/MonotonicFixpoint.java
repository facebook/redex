/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

public class MonotonicFixpoint {
  /**
   *
   * CFG:
   * B0 succs: B1 preds:
   * B1 succs: B1 B2 preds: B0 B1
   * B2 succs: preds: B1
   *   Block B0: --- live in: {}  live out: {v0, v2}
   *     DEBUG: DBG_SET_PROLOGUE_END
   *     POSITION: MonotonicFixpoint.java:45
   *     OPCODE: [0x7fd20de00230] CONST_4 v0
   *     DEBUG: DBG_START_LOCAL v0 a:I
   *     OPCODE: [0x7fd20de002b0] CONST_4 v1
   *     DEBUG: DBG_START_LOCAL v1 b:I
   *     OPCODE: [0x7fd20de00330] CONST_4 v2
   *   Block B1: --- live in: {v0, v2}  live out: {v0, v2}
   *     TARGET SIMPLE: 0x7fd20de00660
   *     POSITION: MonotonicFixpoint.java:48 --- live in/out: {v0, v2}
   *     DEBUG: DBG_START_LOCAL v2 c:I
   *     OPCODE: [0x7fd20de003e0] ADD_INT_LIT8 v1, v0
   *     POSITION: MonotonicFixpoint.java:49 --- live in/out: {v1, v2}
   *     OPCODE: [0x7fd20de00460] ADD_INT v2, v2, v1
   *     POSITION: MonotonicFixpoint.java:50 --- live in/out: {v1, v2}
   *     OPCODE: [0x7fd20de00500] MUL_INT_LIT8 v0, v1
   *     POSITION: MonotonicFixpoint.java:51 --- live in/out: {v0, v2}
   *     OPCODE: [0x7fd20de005e0] CONST_16 v3
   *     OPCODE: [0x7fd20de00660] IF_LT v0, v3
   *   Block B2: --- live in: {v2}  live out: {}
   *     POSITION: MonotonicFixpoint.java:52
   *     OPCODE: [0x7fd20de006e0] RETURN v2
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
