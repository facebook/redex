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

  private static final boolean sUseBinaryIncrementer = false;

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
  public static boolean onMethodExit(int offset, short bitvec) {
    if (sIsEnabled) {
      sMethodStats[offset + 2] |= bitvec;
      return true;
    }
    return false;
  }

  @DoNotStrip
  public static boolean onMethodExit(int offset, short bitvec1, short bitvec2) {
    if (sIsEnabled) {
      sMethodStats[offset + 2] |= bitvec1;
      sMethodStats[offset + 3] |= bitvec2;
      return true;
    }
    return false;
  }

  @DoNotStrip
  public static boolean onMethodExit(int offset, short bitvec1, short bitvec2, short bitvec3) {
    if (sIsEnabled) {
      sMethodStats[offset + 2] |= bitvec1;
      sMethodStats[offset + 3] |= bitvec2;
      sMethodStats[offset + 4] |= bitvec3;
      return true;
    }
    return false;
  }

@DoNotStrip
  public static void onMethodExitUnchecked(int offset, short bitvec) {
    sMethodStats[offset + 2] |= bitvec;
  }

  @DoNotStrip
  public static void onMethodExitUnchecked(int offset, short bitvec1, short bitvec2) {
    sMethodStats[offset + 2] |= bitvec1;
    sMethodStats[offset + 3] |= bitvec2;
  }

  @DoNotStrip
  public static void onMethodExitUnchecked(int offset, short bitvec1, short bitvec2, short bitvec3) {
    sMethodStats[offset + 2] |= bitvec1;
    sMethodStats[offset + 3] |= bitvec2;
    sMethodStats[offset + 4] |= bitvec3;
  }

  // Compared to the previous implementation, we unrolled the loop into a binary tree to
  // find the bits that are set. Basically, check 8 bits then 4 bits then 2 bits then the
  // specific bits. If the lower 8 bits are zero then we can completely ignore incrementing
  // the hit array for the eight blocks with one check rather than doing 8 checks.
  @DoNotStrip
  private static void binaryIncrementer(int offset, short bitvec) {
    short[] hitstats = sHitStats;
    if ((bitvec & 0b11111111) != 0) {
      if ((bitvec & 0b1111) != 0) {
        if ((bitvec & 0b11) != 0) {
          if ((bitvec & 0b1) != 0) {
            hitstats[offset] += 1;
          }

          if ((bitvec & 0b10) != 0) {
            hitstats[offset + 1] += 1;
          }
        }

        if ((bitvec & 0b1100) != 0) {
          if ((bitvec & 0b100) != 0) {
            hitstats[offset + 2] += 1;
          }

          if ((bitvec & 0b1000) != 0) {
            hitstats[offset + 3] += 1;
          }
        }
      }

      if ((bitvec & 0b11110000) != 0) {
        if ((bitvec & 0b110000) != 0) {
          if ((bitvec & 0b10000) != 0) {
            hitstats[offset + 4] += 1;
          }

          if ((bitvec & 0b100000) != 0) {
            hitstats[offset + 5] += 1;
          }
        }

        if ((bitvec & 0b11000000) != 0) {
          if ((bitvec & 0b1000000) != 0) {
            hitstats[offset + 6] += 1;
          }

          if ((bitvec & 0b10000000) != 0) {
            hitstats[offset + 7] += 1;
          }
        }
      }
    }

    if ((bitvec & 0b1111111100000000) != 0) {
      if ((bitvec & 0b111100000000) != 0) {
        if ((bitvec & 0b1100000000) != 0) {
          if ((bitvec & 0b100000000) != 0) {
            hitstats[offset + 8] += 1;
          }

          if ((bitvec & 0b1000000000) != 0) {
            hitstats[offset + 9] += 1;
          }
        }

        if ((bitvec & 0b110000000000) != 0) {
          if ((bitvec & 0b10000000000) != 0) {
            hitstats[offset + 10] += 1;
          }

          if ((bitvec & 0b100000000000) != 0) {
            hitstats[offset + 11] += 1;
          }
        }
      }

      if ((bitvec & 0b1111000000000000) != 0) {
        if ((bitvec & 0b11000000000000) != 0) {
          if ((bitvec & 0b1000000000000) != 0) {
            hitstats[offset + 12] += 1;
          }

          if ((bitvec & 0b10000000000000) != 0) {
            hitstats[offset + 13] += 1;
          }
        }

        if ((bitvec & 0b1100000000000000) != 0) {
          if ((bitvec & 0b100000000000000) != 0) {
            hitstats[offset + 14] += 1;
          }

          if ((bitvec & 0b1000000000000000) != 0) {
            hitstats[offset + 15] += 1;
          }
        }
      }
    }
  }

  @DoNotStrip
  public static void onNonLoopBlockHit(int offset, short bitvec1) {
    if (sIsEnabled) {
      if (sUseBinaryIncrementer) {
        InstrumentBasicBlockAnalysis.binaryIncrementer(offset, bitvec1);
      } else {
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
  }

  @DoNotStrip
  public static void onNonLoopBlockHit(int offset, short bitvec1, short bitvec2) {
    if (sIsEnabled) {
      if (sUseBinaryIncrementer) {
        InstrumentBasicBlockAnalysis.binaryIncrementer(offset, bitvec1);
        InstrumentBasicBlockAnalysis.binaryIncrementer(offset + 16, bitvec2);
      } else {
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
  }

  @DoNotStrip
  public static void onNonLoopBlockHit(int offset, short bitvec1, short bitvec2, short bitvec3) {
    if (sIsEnabled) {
      if (sUseBinaryIncrementer) {
        InstrumentBasicBlockAnalysis.binaryIncrementer(offset, bitvec1);
        InstrumentBasicBlockAnalysis.binaryIncrementer(offset + 16, bitvec2);
        InstrumentBasicBlockAnalysis.binaryIncrementer(offset + 32, bitvec3);
      } else {
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
  }

  @DoNotStrip
  public static void onBlockHit(int offset) {
    if (sIsEnabled) {
      sHitStats[offset] += 1;
    }
  }
}
