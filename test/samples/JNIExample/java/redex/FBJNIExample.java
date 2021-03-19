// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.
package FBJNIExample;

import com.facebook.soloader.DoNotOptimize;
import com.facebook.soloader.SoLoader;
import com.facebook.soloader.annotation.SoLoaderLibrary;

@SoLoaderLibrary("Animal")
@DoNotOptimize
public class FBJNIExample {
  static {
    SoLoader.loadLibrary("Animal");
  }
  public static native String implemented(String name, int value);
  public static native String missing(String name, int value);
}
