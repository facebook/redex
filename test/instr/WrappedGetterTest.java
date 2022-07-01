/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Ignore;
import org.junit.Test;

public class WrappedGetterTest {

    /**
     * Test that accessing a static final variable via a wrapper works
     * correctly.
     */
    @Test
    public void test() {
        assertThat(wrap().getClass().getSimpleName()).isEqualTo("WrappedGetterTest");
    }

    private static WrappedGetterTest wrap() {
        return Bar.statb;
    }

    private static class Bar {
        private static final WrappedGetterTest statb = new WrappedGetterTest();
    }
}
