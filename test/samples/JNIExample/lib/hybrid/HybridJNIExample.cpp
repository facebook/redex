/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HybridJNIExample.h"

#include <fb/xplat_init.h>

namespace facebook {

using namespace facebook::jni;

HybridJNIExample::HybridJNIExample(int i) : i_(i) {}
HybridJNIExample::~HybridJNIExample() {}

void HybridJNIExample::registerNatives() {
  registerHybrid({
      makeNativeMethod("initHybrid", HybridJNIExample::initHybrid),
      makeNativeMethod("implementedRegisteredDeclaredUsed",
                       HybridJNIExample::implementedRegisteredDeclaredUsed),
      makeNativeMethod("implementedRegisteredDeclared",
                       HybridJNIExample::implementedRegisteredDeclared),
      //      makeNativeMethod("implementedRegistered",
      //                       HybridJNIExample::implementedRegistered),
  });
}

local_ref<HybridJNIExample::jhybriddata> HybridJNIExample::initHybrid(
    jni::alias_ref<jclass>, const int i) {
  return makeCxxInstance(i);
}

int HybridJNIExample::implementedRegisteredDeclaredUsed() { return 1; }
int HybridJNIExample::implementedRegisteredDeclared() { return 1; }
int HybridJNIExample::implementedRegistered() { return 1; }

} // namespace facebook

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::xplat::initialize(
      vm, [] { facebook::HybridJNIExample::registerNatives(); });
}
