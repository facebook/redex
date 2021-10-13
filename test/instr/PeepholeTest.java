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

interface AnonInterface {}

public class PeepholeTest {
    // CHECK: method: {{.*}} redex.PeepholeTest.testGetInnerClassName:
    @Test
    public void testGetInnerClassName() {
        OuterClass.InnerClass[] InnerArray = new OuterClass.InnerClass[4];
        // PRECHECK: const-class {{.*}} redex.OuterClass
        // POSTCHECK-NOT: const-class {{.*}} redex.OuterClass
        assertThat(OuterClass.class.getSimpleName()).isEqualTo("OuterClass");
        // PRECHECK: const-class {{.*}} redex.OuterClass$InnerClass
        // POSTCHECK-NOT: const-class {{.*}} redex.OuterClass$InnerClass
        assertThat(OuterClass.InnerClass.class.getSimpleName()).isEqualTo("InnerClass");
        assertThat(InnerArray.getClass().getSimpleName()).isEqualTo("InnerClass[]");
    }

    // CHECK: method: {{.*}} redex.PeepholeTest.testGetAnonymousClassName:
    @Test
    public void testGetAnonymousClassName() {
        Object anonInstance = new AnonInterface() {};
        assertThat(anonInstance.getClass().getSimpleName()).isEqualTo("");
    }
}
