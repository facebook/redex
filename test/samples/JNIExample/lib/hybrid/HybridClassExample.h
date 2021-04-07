/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fb/fbjni.h>

namespace facebook {
class HybridClassExampleJni final
    : public jni::HybridClass<HybridClassExampleJni> {
 public:
  constexpr static auto kJavaDescriptor = "Lredex/HybridClassExample;";

  explicit HybridClassExampleJni(int i);

  ~HybridClassExampleJni();

  static facebook::jni::local_ref<jhybriddata> initHybrid(
      jni::alias_ref<jclass>, const int i);

  static void registerNatives();

 public:
  int i_;
};
} // namespace facebook
