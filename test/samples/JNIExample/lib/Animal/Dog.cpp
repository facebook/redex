/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Dog.h"
#include <iostream>
#include <sstream>
#include <string>

extern "C" int puts(char const*);

void Dog::makeNoise() { puts("Woof"); }

void doThingWithDog(Animal* a) {
  if (auto* d = dynamic_cast<Dog*>(a)) d->makeNoise();
}

namespace facebook {
namespace redex {
namespace samples {

std::string implementedButUnused(facebook::jni::alias_ref<jobject> thiz,
                                 int value) {
  std::cout << value << std::endl;
  return std::to_string(value);
}

std::string implemented(facebook::jni::alias_ref<jobject> thiz,
                        std::string name,
                        int value) {
  std::stringstream ret;
  ret << "libAnimal.so::Dog.cpp::implemented -- name: " << name
      << ", value: " << value;

  return ret.str();
}

std::string unused(facebook::jni::alias_ref<jobject> thiz,
                   std::string name,
                   int value) {
  std::cout << name << std::endl;
  return std::to_string(value);
}

} // namespace samples
} // namespace redex

} // namespace facebook
