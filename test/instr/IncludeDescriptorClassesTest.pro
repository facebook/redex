-keepclasseswithmembers,includedescriptorclasses class redex.IncludeDescriptorClassesTest {
    ** preservedField;
    void methodWithPreservedArgs(...);
}

-keepclasseswithmembers class redex.IncludeDescriptorClassesTest {
    ** renamedField;
    void methodWithRenamedArgs(...);
}
