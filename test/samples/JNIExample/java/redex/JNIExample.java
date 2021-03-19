/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

public class JNIExample {
  static {
    System.loadLibrary("jniexample");
  }

  public native void missing();
  public native void implemented();
}
