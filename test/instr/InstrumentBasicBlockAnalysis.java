/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * <p>This source code is licensed under the MIT license found in the LICENSE file in the root
 * directory of this source tree.
 */
package com.facebook.redextest;

import java.util.HashMap;

public class InstrumentBasicBlockAnalysis {

  private static final boolean[] sBasicBlockStats = new boolean[0];

  public static void onBasicBlockBegin(int bb_index) {
    sBasicBlockStats[bb_index] = true;
  }
}
