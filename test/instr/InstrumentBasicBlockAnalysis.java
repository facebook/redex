/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * <p>This source code is licensed under the MIT license found in the LICENSE file in the root
 * directory of this source tree.
 */
package com.facebook.redextest;

import java.util.HashMap;

public class InstrumentBasicBlockAnalysis {

  private static final HashMap<Integer, Integer> bbMap = new HashMap<>();

  public static void onBasicBlockBegin(int blockId) {
    bbMap.put(blockId, 1);
  }
}
