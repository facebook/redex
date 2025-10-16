/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex

// Test static method
// CHECK: {{.*}}redex.RemoveNullcheckStringArgTestKt.staticMethodWithNullCheck:{{.*}}STATIC{{.*}}
// The null check should be rewritten and the param index is 0
// PRECHECK: const-string v{{[0-9]+}}, "name"
// PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
// POSTCHECK-NOT: const-string v{{[0-9]+}}, "name"
// POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
// POSTCHECK: const/4 v{{[0-9]+}}, #int 0
// POSTCHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.$WrCheckParameter_V1_
// CHECK: const-string v{{[0-9]+}}, "anchor3, "
// CHECK: return-void
fun staticMethodWithNullCheck(name: String) {
  print("anchor3, $name!")
}

// Test virtual method in a class
class TestClass {
  // CHECK: method: virtual redex.TestClass.virtualMethodWithNullCheck:
  // The null check should be rewritten and the param index is 0
  // PRECHECK: const-string v{{[0-9]+}}, "message"
  // PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
  // POSTCHECK-NOT: const-string v{{[0-9]+}}, "message"
  // POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
  // POSTCHECK: const/4 v{{[0-9]+}}, #int 0
  // POSTCHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.$WrCheckParameter_V1_
  // CHECK: const-string v{{[0-9]+}}, "anchor1: "
  // CHECK: return-void
  fun virtualMethodWithNullCheck(message: String) {
    print("anchor1: $message")
  }
}

// Test constructor
class TestClassWithConstructor(age: Int, name: String) {
  // CHECK: method: direct redex.TestClassWithConstructor.<init>:(int, java.lang.String)
  // The null check should be rewritten and the param index is 1
  // PRECHECK: const-string v{{[0-9]+}}, "name"
  // PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
  // POSTCHECK-NOT: const-string v{{[0-9]+}}, "name"
  // POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
  // POSTCHECK: const/4 v{{[0-9]+}}, #int 1
  // POSTCHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.$WrCheckParameter_V1_
  // CHECK: iput-object {{.*}} redex.TestClassWithConstructor.anchor2:
  // CHECK: return-void
  val anchor2: String = name
  val ageField: Int = age

  fun printInfo() {
    print("Name: $anchor2, Age: $ageField")
  }
}

// Test @JVMStatic method in companion object
class TestClassWithCompanion {
  companion object {
    // CHECK: {{.*}}redex.TestClassWithCompanion.jvmStaticMethodWithNullCheck:{{.*}}STATIC{{.*}}
    // The null check should be rewritten and the param index is 0
    // PRECHECK: const-string v{{[0-9]+}}, "value"
    // PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
    // POSTCHECK-NOT: const-string v{{[0-9]+}}, "value"
    // POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.checkNotNullParameter:
    // POSTCHECK: const/4 v{{[0-9]+}}, #int 0
    // POSTCHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.$WrCheckParameter_V1_
    // CHECK: const-string v{{[0-9]+}}, "anchor4: "
    // CHECK: return-void
    @JvmStatic
    fun jvmStaticMethodWithNullCheck(value: String) {
      print("anchor4: $value")
    }
  }
}

fun testMethods() {
  staticMethodWithNullCheck("World")

  val testObj = TestClass()
  testObj.virtualMethodWithNullCheck("Hello from virtual")

  val constructorObj = TestClassWithConstructor(30, "Alice")
  constructorObj.printInfo()

  TestClassWithCompanion.jvmStaticMethodWithNullCheck("JVMStatic call")
}
