# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

-keepclasseswithmembers,includedescriptorclasses class redex.IncludeDescriptorClassesTest {
    ** preservedField;
    void methodWithPreservedArgs(...);
}

-keepclasseswithmembers class redex.IncludeDescriptorClassesTest {
    ** renamedField;
    void methodWithRenamedArgs(...);
}
