/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.assertj.core.api.Assertions.*;

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
