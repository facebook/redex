/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

// PRECHECK: class: redex.PreservedFieldClass
// POSTCHECK: class: redex.PreservedFieldClass
class PreservedFieldClass {
}

// PRECHECK: class: redex.RenamedFieldClass
// POSTCHECK-NOT: class: redex.RenamedFieldClass
class RenamedFieldClass {
}

// PRECHECK: class: redex.PreservedArgClass
// POSTCHECK: class: redex.PreservedArgClass
class PreservedArgClass {
}

// PRECHECK: class: redex.RenamedArgClass
// POSTCHECK-NOT: class: redex.RenamedArgClass
class RenamedArgClass {
}

public class IncludeDescriptorClassesTest {
    PreservedFieldClass preservedField;
    RenamedFieldClass renamedField;

    // PRECHECK: method: virtual redex.IncludeDescriptorClassesTest.methodWithPreservedArgs:(redex.PreservedArgClass)void (PUBLIC)
    // POSTCHECK: method: virtual redex.IncludeDescriptorClassesTest.methodWithPreservedArgs:(redex.PreservedArgClass)void (PUBLIC)
    public void methodWithPreservedArgs(PreservedArgClass arg) {
    }

    // PRECHECK: method: virtual redex.IncludeDescriptorClassesTest.methodWithRenamedArgs:(redex.RenamedArgClass)void (PUBLIC)
    // POSTCHECK-NOT: method: virtual redex.IncludeDescriptorClassesTest.methodWithRenamedArgs:(redex.RenamedArgClass)void (PUBLIC)
    public void methodWithRenamedArgs(RenamedArgClass arg) {
    }
}
