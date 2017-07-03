/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

public class AllocInvokeRangeTest {

    /* Required to prevent redex from deleting constructor, needed for JUnit */
    @Test
    public void createTestObject() {
        new AllocInvokeRangeTest();
    }

    /**
     * TODO: Document this test!!!
     */
    static String result;
    @Test
    public void test() {
        result = null;
        int a = 1;
        int b = 2;
        int c = 3;
        int d = 4;
        int e = 5;
        int f = 6;
        invoke1(a, b, c, d, e, f);
        assertThat(result).isEqualTo("concat: 654321");
    }

    private static void invoke1(int a, int b, int c, int d, int e, int f) {
        invoke2(f, e, d, c, b, a);
    }

    private static void invoke2(int a, int b, int c, int d, int e, int f) {
        result = "concat: " + a + b + c + d + e + f;
    }
}
