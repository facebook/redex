/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Dog.h>
#include <fb/fbjni.h>

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  return facebook::jni::initialize(vm, [] {
    facebook::jni::registerNatives(
        "com/facebook/redex/samples",
        {
            makeNativeMethod("implemented",
                             facebook::redex::samples::implemented),
            makeNativeMethod("unused", facebook::redex::samples::unused),
        });
  });
}
