/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import com.facebook.proguard.annotations.DoNotStrip;
import java.util.concurrent.atomic.AtomicInteger;

@DoNotStrip
public class InstrumentBasicBlockAnalysis {

  // InstrumentPass will patch.
  @DoNotStrip private static final short[] sMethodStats = new short[0];
  @DoNotStrip private static short[][] sMethodStatsArray = new short[][] {}; // Redex will patch
  @DoNotStrip private static int sNumStaticallyInstrumented = 0;
  @DoNotStrip private static int sProfileType = 0;

  @DoNotStrip private static boolean sIsEnabled = true;
  @DoNotStrip private static AtomicInteger sMethodCounter = new AtomicInteger(0);

  @DoNotStrip
  public static void onMethodBegin(int offset) {
    if (sIsEnabled) {
      ++sMethodStats[offset];
      if (sMethodStats[offset + 1] == 0) {
        sMethodStats[offset + 1] = (short) sMethodCounter.incrementAndGet();
      }
    }
  }

  @DoNotStrip
  public static void onMethodExit(int offset, short bitvec) {
    if (sIsEnabled) {
      sMethodStats[offset + 2] |= bitvec;
    }
  }

  @DoNotStrip
  public static void onMethodExit(int offset, short bitvec1, short bitvec2) {
    if (sIsEnabled) {
      sMethodStats[offset + 2] |= bitvec1;
      sMethodStats[offset + 3] |= bitvec2;
    }
  }

  @DoNotStrip
  public static void onMethodExit(int offset, short bitvec1, short bitvec2, short bitvec3) {
    if (sIsEnabled) {
      sMethodStats[offset + 2] |= bitvec1;
      sMethodStats[offset + 3] |= bitvec2;
      sMethodStats[offset + 4] |= bitvec3;
    }
  }
}
