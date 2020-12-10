/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import com.facebook.proguard.annotations.DoNotStrip;

import android.util.Log;
import java.text.MessageFormat;

public class InstrumentAnalysis {
  private static final String LOG_TAG = "DYNA";

  // Redex will patch these fields.
  @DoNotStrip private static final int[] sMethodStats = new int[0];
  @DoNotStrip private static short[][] sMethodStatsArray = new short[][] {};
  @DoNotStrip private static int sNumStaticallyInstrumented = 0;
  @DoNotStrip private static int sProfileType = 0;

  @DoNotStrip
  public static void onMethodBegin(int index) {
    ++sMethodStats[index];
    Log.i(LOG_TAG, MessageFormat.format("increment: {0}, {1}", index, sMethodStats[index]));
  }
}
