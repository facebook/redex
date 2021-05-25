/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex.jni.example;

import com.facebook.soloader.DoNotOptimize;

@DoNotOptimize
public class BasicJNIExample {
  static {
    System.loadLibrary("basicjniexample");
  }

  public int doThing() {
    int a = implementedRegisteredDeclaredUsed();
    return a;
  }

  public native int implementedRegisteredDeclaredUsed();
  public native int implementedRegisteredDeclared();
}
