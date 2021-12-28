/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT int JNICALL
Java_redex_jni_example_BasicJNIExample_implementedRegisteredDeclaredUsed(
    JNIEnv*, jobject);
JNIEXPORT int JNICALL
Java_redex_jni_example_BasicJNIExample_implementedRegisteredDeclared(JNIEnv*,
                                                                     jobject);
JNIEXPORT int JNICALL
Java_redex_jni_example_BasicJNIExample_implementedRegistered(JNIEnv*, jobject);

#ifdef __cplusplus
}
#endif
