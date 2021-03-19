/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Dog.h"
#include "redex_JNIExample.h"

int foo() {
  auto* d = new Dog;
  doThingWithDog(d);
  return 1;
}

extern "C" int puts(char const*);

JNIEXPORT void JNICALL Java_redex_JNIExample_implemented(JNIEnv*, jobject) {
  puts("hi");
  foo();
}

JNIEXPORT void JNICALL Java_redex_JNIExample_unused(JNIEnv*, jobject) {
  puts("hi");
  foo();
}
