/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fb/fbjni.h>

namespace facebook {
class HybridJNIExample final : public jni::HybridClass<HybridJNIExample> {
 public:
  constexpr static auto kJavaDescriptor =
      "Lredex/jni/example/HybridJNIExample;";

  explicit HybridJNIExample(int i);

  ~HybridJNIExample();

  static facebook::jni::local_ref<jhybriddata> initHybrid(
      jni::alias_ref<jclass>, const int i);

  int implementedRegisteredDeclaredUsed();
  int implementedRegisteredDeclared();
  int implementedRegistered();

  static void registerNatives();

 public:
  int i_;
};
} // namespace facebook
