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

public class Synth2Test {

    /**
     * This tests some subtle behavior in synth removal, to make sure that
     * Inner.a() isn't visited twice in the same pass.  What could happen is
     * that it gets processed once; then it gets changed to a static in the
     * course of processing Inner.a(Aaa), so it then gets visited again as
     * Inner.a(Inner), which trips an assert when it tries to optimize the call
     * to Inner.b() a second time around.
     */
    @Test
    public void test() {
        Inner inner = new Inner();
        Aaa aaa = new Aaa();
        int ab = inner.a() + inner.b() + inner.a(aaa);
        assertThat(ab).isEqualTo(42 * 7);
    }

    private class Aaa {
    }

    private class Inner {
        private int a() {
            return 3 * b();
        }

        private int a(Aaa b) {
            return a();
        }

        private int b() {
            return 42;
        }
    }
}
