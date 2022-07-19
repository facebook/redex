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

  @DoNotStrip private static int[] sHitStats = new int[0];
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
  public static int[] getHitStats() {
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

  @DoNotStrip
  public static void onBlockHit(int offset) {
    if (sIsEnabled) {
      sHitStats[offset] += 1;
    }
  }
}
