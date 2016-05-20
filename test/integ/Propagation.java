/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */


/**
 * This Java class is used to test the LocalDCE optimization.
 * The test uses Peephole followed by LocalDCE.
 *
 * Code before:
 * dmethod: regs: 2, ins: 0, outs: 1
 * const-class Lcom/facebook/redextest/Propagation; v1
 * invoke-virtual java.lang.Class.getSimpleName()Ljava/lang/String; v1
 * move-result-object v0
 * return-object v0
 *
 * Expected code after:
 * dmethod: regs: 2, ins: 0, outs: 1
 * const-string Propagation v0
 * return-object v0
 *
 * Note that the invoke-virtual call to java.lang.Class.getSimpleName() has
 * been removed.
 */

package com.facebook.redextest;

public class Propagation {
    public static String propagate() {
        String tag = com.facebook.redextest.Propagation.class.getSimpleName();
        return tag;
    }
}
