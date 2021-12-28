/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

public class ReferencedDeadCodeTest {

    /**
     * TODO: Document this test!!!
     * TODO: This test is currently broken!!! It doesn't fail when you
     *       remove @KeepForRedexTest. And it probably shouldn't, but
     *       this test just doesn't seem to really prove anything.
     */
    @Test
    public void test() {
        ReferenceInDeadCode ridc = new ReferenceInDeadCode();
    }

    private interface NeverInitInterface {
        public void neverCalled();
    }

    @KeepForRedexTest
    private class NeverInit implements NeverInitInterface {
        public NeverInit() {
        }

        public void neverCalled() {
        }
    }

    private class ReferenceInDeadCode {
        public ReferenceInDeadCode() {
        }

        @KeepForRedexTest
        public void referenceDeadCode(NeverInitInterface ni) {
            ni.neverCalled();
        }
    }
}
