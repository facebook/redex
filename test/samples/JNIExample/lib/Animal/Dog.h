/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "Animal.h"
#include <fb/fbjni.h>
#include <string>

struct Dog : public Animal {
  void makeNoise() override;
};

void doThingWithDog(Animal* a);

namespace facebook {
namespace redex {
namespace samples {
std::string implemented(facebook::jni::alias_ref<jobject> thiz,
                        std::string name,
                        int value);

std::string unused(facebook::jni::alias_ref<jobject> thiz,
                   std::string name,
                   int value);
} // namespace samples
} // namespace redex
} // namespace facebook
