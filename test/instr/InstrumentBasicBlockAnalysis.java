/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import com.facebook.proguard.annotations.DoNotStrip;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.Arrays;

@DoNotStrip
public class InstrumentBasicBlockAnalysis {

  // InstrumentPass will patch.
  @DoNotStrip private static final short[] sMethodStats = new short[0];
  @DoNotStrip private static short[][] sMethodStatsArray = new short[][] {}; // Redex will patch
  @DoNotStrip private static int sNumStaticallyInstrumented = 0;
  @DoNotStrip private static int sProfileType = 0;

  @DoNotStrip private static short[] sHitStats = new short[0];
  @DoNotStrip private static int sNumStaticallyHitsInstrumented = 0;

  @DoNotStrip private static boolean sIsEnabled = true;
  @DoNotStrip private static AtomicInteger sMethodCounter = new AtomicInteger(0);

  @DoNotStrip
  public static void cleanup() {
    sMethodCounter.set(0);

    // Clearing up sMethodStatsArray.
    for (int i = 0; i < sMethodStats.length; ++i) {
      sMethodStats[i] = 0;
    }

    // Clearing up sHitStats.
    for (int i = 0; i < sHitStats.length; ++i) {
      sHitStats[i] = 0;
    }
  }

  @DoNotStrip
  public static void startTracing() {
    InstrumentBasicBlockAnalysis.cleanup();
    sIsEnabled = true;
  }

  @DoNotStrip
  public static void stopTracing() {
    sIsEnabled = false;
  }

  @DoNotStrip
  public static void dumpStats() {
    System.out.println("Vector " + Arrays.toString(sMethodStats));
  }

  @DoNotStrip
  public static short[] getStats() {
    return sMethodStats;
  }

  @DoNotStrip
  public static short[] getHitStats() {
    return sHitStats;
  }


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

  // Compared to the previous implementation, we shift the bitvector after each iteration.
  // The shift is an int operator not short so we make the bitvector an int at the
  // beginning so it does not lead to many casting operations. Through shifting, the
  // bitvector can become zero meaning no more bits are set and the rest of the blocks
  // are not hit. Thus, separating each bitvector to its loop allows early loop exits.
  @DoNotStrip
  public static void onNonLoopBlockHit(int offset, short bitvec1) {
    if (sIsEnabled && sNumStaticallyHitsInstrumented > 0) {
      int bit1 = 0;
      int bitvecI1 = bitvec1;
      for (int i = 0; i < 16; i++) {
        if (bitvecI1 == 0) {
          break;
        }
        bit1 = bitvecI1 & 1;

        if (bit1 != 0) {
          sHitStats[offset + i] += 1;
        }

        bitvecI1 = bitvecI1 >> 1;
      }
    }
  }

  @DoNotStrip
  public static void onNonLoopBlockHit(int offset, short bitvec1, short bitvec2) {
    if (sIsEnabled && sNumStaticallyHitsInstrumented > 0) {
      int bit1 = 0, bit2 = 0;
      int bitvecI1 = bitvec1;
      int bitvecI2 = bitvec2;
      for (int i = 0; i < 16; i++) {
        if (bitvecI1 == 0) {
          break;
        }
        bit1 = bitvecI1 & 1;

        if (bit1 != 0) {
          sHitStats[offset + i] += 1;
        }

        bitvecI1 = bitvecI1 >> 1;
      }

      for (int i = 0; i < 16; i++) {
        if (bitvecI2 == 0) {
          break;
        }
        bit2 = bitvecI2 & 1;

        if (bit2 != 0) {
          sHitStats[offset + i + 16] += 1;
        }

        bitvecI2 = bitvecI2 >> 1;
      }
    }
  }

  @DoNotStrip
  public static void onNonLoopBlockHit(int offset, short bitvec1, short bitvec2, short bitvec3) {
    if (sIsEnabled && sNumStaticallyHitsInstrumented > 0) {
      int bit1 = 0, bit2 = 0, bit3 = 0;
      int bitvecI1 = bitvec1;
      int bitvecI2 = bitvec2;
      int bitvecI3 = bitvec3;
      for (int i = 0; i < 16; i++) {
        if (bitvecI1 == 0) {
          break;
        }
        bit1 = bitvecI1 & 1;

        if (bit1 != 0) {
          sHitStats[offset + i] += 1;
        }

        bitvecI1 = bitvecI1 >> 1;
      }

      for (int i = 0; i < 16; i++) {
        if (bitvecI2 == 0) {
          break;
        }
        bit2 = bitvecI2 & 1;

        if (bit2 != 0) {
          sHitStats[offset + i + 16] += 1;
        }

        bitvecI2 = bitvecI2 >> 1;
      }

      for (int i = 0; i < 16; i++) {
        if (bitvecI3 == 0) {
          break;
        }
        bit3 = bitvecI3 & 1;

        if (bit3 != 0) {
          sHitStats[offset + i + 32] += 1;
        }

        bitvecI3 = bitvecI3 >> 1;
      }
    }
  }

  @DoNotStrip
  public static void onBlockHit(int offset) {
    if (sIsEnabled) {
      sHitStats[offset] += 1;
    }
  }
}
