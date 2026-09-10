/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import integ.TestIntDef;

interface TypedefInterface {
    @TestIntDef int transform(@TestIntDef int val);
}

class TypedefImpl implements TypedefInterface {
    @TestIntDef
    public int transform(@TestIntDef int val) {
        return val;
    }
}

// Simulates Kotlin delegation: the forwarding method intentionally lacks
// typedef annotations — the patcher should propagate them from the interface.
class DelegatingClass implements TypedefInterface {
    // Named $$delegate_0 to match Kotlin's delegation field naming.
    TypedefInterface $$delegate_0;

    DelegatingClass(TypedefInterface delegate) {
        this.$$delegate_0 = delegate;
    }

    public int transform(int val) {
        return $$delegate_0.transform(val);
    }
}

class DelegateTestHelper {
    static void callDelegate(@TestIntDef int val) {
        DelegatingClass d = new DelegatingClass(new TypedefImpl());
        d.transform(val);
    }
}
