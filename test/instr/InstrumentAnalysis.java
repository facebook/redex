/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import android.util.Log;
import java.text.MessageFormat;

public class InstrumentAnalysis {
  private static final String LOG_TAG = "DYNA";

  private static int sMethodCount = 0; // Redex will patch
  private static final int[] sMethodStats1 = new int[0]; // Redex will patch
  private static short[][] sMethodStatsArray = new short[][] {};  // Redex will patch

  public static void onMethodBegin1(int index) {
    ++sMethodStats1[index];
    Log.i(LOG_TAG, MessageFormat.format("increment: {0}, {1}", index, sMethodStats1[index]));
  }
}
