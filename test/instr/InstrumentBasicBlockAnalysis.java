/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * <p>This source code is licensed under the MIT license found in the LICENSE file in the root
 * directory of this source tree.
 */
package com.facebook.redextest;

public class InstrumentBasicBlockAnalysis {

  private static final int[] sBasicBlockStats = new int[0];

  public static void onMethodExitBB(int methodId, int bbVector) {
    sBasicBlockStats[methodId] = bbVector;
  }
}
