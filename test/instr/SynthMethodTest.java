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

public class SynthMethodTest {

    /**
     * TODO: Document this test!!!
     */
    @Test
    public void test() {
        Inner inner = new Inner();
        int observed = inner.someMethod();
        assertThat(observed).isEqualTo(12);
    }

    private class Inner {
        private int someMethod() {
            return 12;
        }
    }
}
