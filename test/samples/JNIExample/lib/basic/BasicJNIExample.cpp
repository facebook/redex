/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BasicJNIExample.h"

JNIEXPORT jint JNICALL
Java_redex_jni_example_BasicJNIExample_implementedRegisteredDeclaredUsed(
    JNIEnv*, jobject) {
  return 1;
}

JNIEXPORT jint JNICALL
Java_redex_jni_example_BasicJNIExample_implementedRegisteredDeclared(JNIEnv*,
                                                                     jobject) {
  return 1;
}

JNIEXPORT jint JNICALL
Java_redex_jni_example_BasicJNIExample_implementedRegistered(JNIEnv*, jobject) {
  return 1;
}
