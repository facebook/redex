/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * <p>This source code is licensed under the MIT license found in the LICENSE file in the root
 * directory of this source tree.
 */
package com.facebook.redextest;

import java.util.HashMap;

public class InstrumentBBAnalysis {

  private static final HashMap<Integer, Integer> bb_map = new HashMap<>();

  public static void on_bb_begin(int block_id) {
    bb_map.put(block_id, 1);
  }
}
