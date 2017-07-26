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

import org.junit.Ignore;
import org.junit.Test;

public class SynthWrapperCollisionTest {

    static class Inner {
        int mData;

        public Inner(int data) {
            mData = data;
        }

        public static int doubleData(Inner i) {
            return i.doubleData();
        }

        private int doubleData() {
            return mData * 2;
        }

        private int quadData() {
            return 2 * doubleData();
        }
    }

    /**
     * Test that synth optimization works properly when promotion of a wrapped
     * method to a public static is blocked by a pre-existing method (see
     * doubleData() and doubleData(Inner i), above).
     */
    @Test
    public void test() {
        Inner i1 = new Inner(1);
        assertThat(Inner.doubleData(i1)).isEqualTo(2);
        assertThat(i1.doubleData()).isEqualTo(2);
        assertThat(i1.quadData()).isEqualTo(4);
        Inner i2 = new Inner(2);
        assertThat(Inner.doubleData(i2)).isEqualTo(4);
        assertThat(i2.doubleData()).isEqualTo(4);
        assertThat(i2.quadData()).isEqualTo(8);
    }
}
