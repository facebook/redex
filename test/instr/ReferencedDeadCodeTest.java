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
