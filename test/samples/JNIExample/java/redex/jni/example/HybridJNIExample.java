/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex.jni.example;

import com.facebook.jni.HybridData;
import com.facebook.soloader.DoNotOptimize;
import com.facebook.proguard.annotations.DoNotStrip;
import com.facebook.soloader.SoLoader;

public class HybridJNIExample {
  static {
    SoLoader.loadLibrary("hybridjniexample");
  }

  @DoNotStrip private final HybridData mHybridData;

  public int doThing() {
    int a = implementedRegisteredDeclaredUsed();
    return a;
  }

  public HybridJNIExample() {
    mHybridData = initHybrid(0);
  }

  private static native HybridData initHybrid(int i);

  public native int implementedRegisteredDeclaredUsed();

  public native int implementedRegisteredDeclared();
}
