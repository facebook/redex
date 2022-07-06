/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <SimpleJni/common.h>
#include <SimpleJni/javaclasses.h>

namespace facebook::jni::redexsimplejniexample {

int implementedRegisteredDeclaredUsed(JNIEnv* env) { return 1; }

int implementedRegisteredDeclared(JNIEnv* env) { return 1; }

int implementedRegistered(JNIEnv* env) { return 1; }

static JNINativeMethod methods[] = {
    {"implementedRegisteredDeclaredUsed", "()I",
     (void*)implementedRegisteredDeclaredUsed},
    {"implementedRegisteredDeclared", "()I",
     (void*)implementedRegisteredDeclared},
    //    {"implementedRegistered", "()I", (void*)implementedRegistered},
};

static auto constexpr kClassName = "redex/jni/example/SimpleJNIExample";

void registerNatives(JNIEnv* env) {
  ::facebook::simplejni::registerNatives(
      env, kClassName, methods, sizeof(methods) / sizeof(JNINativeMethod));
}

void JNI_registerAll(JNIEnv* env) {
  ::facebook::jni::redexsimplejniexample::registerNatives(env);
}

} // namespace facebook::jni::redexsimplejniexample

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  jint result = ::facebook::simplejni::ensureInitialized(&env, vm);

  ::facebook::jni::redexsimplejniexample::JNI_registerAll(env);

  return result;
}
