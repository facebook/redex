/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Dog.h>
#include <fb/fbjni.h>

// The template arguments here are merely for forwarding to the
// exceptionWrapJNIMethod function
template <typename F, F func0>
JNINativeMethod makeNativeMethod2Impl(char const* name, F func) {
  return {
      const_cast<char*>(name),
      const_cast<char*>(::facebook::jni::detail::makeDescriptor(func).c_str()),
      ::facebook::jni::detail::exceptionWrapJNIMethod<F, func0>(func)};
}

template <typename F, F func0>
JNINativeMethod makeNativeMethod3Impl(char const* name,
                                      char const* desc,
                                      F func) {
  return {const_cast<char*>(name), const_cast<char*>(desc),
          ::facebook::jni::detail::exceptionWrapJNIMethod<F, func0>(func)};
}

// this just wraps the above method so that you don't have to write out the
// `func` argument 3 times
#define makeNativeMethodX(name, func) \
  makeNativeMethod2Impl<decltype(func), func>(name, func)

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  return facebook::jni::initialize(vm, [] {
    facebook::jni::registerNatives(
        "redex/jni/example/Main",
        {
            makeNativeMethodX("implemented",
                              facebook::redex::samples::implemented),
            makeNativeMethodX("implementedButUnused",
                              facebook::redex::samples::implementedButUnused),
        });
  });
}
