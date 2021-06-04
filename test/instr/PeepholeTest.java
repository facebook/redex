/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

class OuterClass {
    class InnerClass {}
}

public class PeepholeTest {
    // CHECK: method: {{.*}} redex.PeepholeTest.testGetInnerClassName:
    @Test
    public void testGetInnerClassName() {
        // PRECHECK: const-class {{.*}} redex.OuterClass
        // POSTCHECK-NOT: const-class {{.*}} redex.OuterClass
        assertThat(OuterClass.class.getSimpleName()).isEqualTo("OuterClass");
        // PRECHECK: const-class {{.*}} redex.OuterClass$InnerClass
        // POSTCHECK-NOT: const-class {{.*}} redex.OuterClass$InnerClass
        assertThat(OuterClass.InnerClass.class.getSimpleName()).isEqualTo("InnerClass");
    }
}
