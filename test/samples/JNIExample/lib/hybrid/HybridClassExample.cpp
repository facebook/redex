/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HybridClassExample.h"

#include <fb/xplat_init.h>

namespace facebook {

using namespace facebook::jni;

HybridClassExampleJni::HybridClassExampleJni(int i) : i_(i) {}

HybridClassExampleJni::~HybridClassExampleJni() {}

void HybridClassExampleJni::registerNatives() {
  registerHybrid({
      makeNativeMethod("initHybrid", HybridClassExampleJni::initHybrid),
  });
}

local_ref<HybridClassExampleJni::jhybriddata> HybridClassExampleJni::initHybrid(
    jni::alias_ref<jclass>, const int i) {
  return makeCxxInstance(i);
}
} // namespace facebook

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::xplat::initialize(
      vm, [] { facebook::HybridClassExampleJni::registerNatives(); });
}
