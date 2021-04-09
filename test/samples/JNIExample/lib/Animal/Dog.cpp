#include "Dog.h"
#include <iostream>
#include <string>

extern "C" int puts(char const*);

void Dog::makeNoise() { puts("Woof"); }

void doThingWithDog(Animal* a) {
  if (auto* d = dynamic_cast<Dog*>(a)) d->makeNoise();
}

namespace facebook {
namespace redex {
namespace samples {
std::string implemented(facebook::jni::alias_ref<jobject> thiz,
                        std::string name,
                        int value) {
  std::cout << name << std::endl;
  return std::to_string(value);
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
