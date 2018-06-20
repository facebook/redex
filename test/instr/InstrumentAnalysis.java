/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

import android.util.Log;
import java.text.MessageFormat;

public class InstrumentAnalysis {
  private static final String LOG_TAG = "DYNA";

  private static int sMethodCount = 19; // Redex will patch
  private static final int[] sStats = new int[17]; // Redex will patch

  public static void onMethodBegin(int index) {
    ++sStats[index];
    Log.i(LOG_TAG, MessageFormat.format("increment: {0}, {1}", index, sStats[index]));
  }
}
