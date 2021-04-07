/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import com.facebook.jni.HybridData;
import com.facebook.proguard.annotations.DoNotStrip;
import com.facebook.soloader.SoLoader;

public class HybridClassExample {
  static {
    SoLoader.loadLibrary("libhybridclassexample.so");
  }

  @DoNotStrip private final HybridData mHybridData;

  public HybridClassExample(int i) {
    mHybridData = initHybrid(i);
  }

  private static native HybridData initHybrid(int i);
}
