/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class InstrumentBasicBlockAnalysis {

  // InstrumentPass will patch.
  private static final short[] sBasicBlockStats = new short[0];

  // InstrumentPass will insert one of these onMethodExit calls.
  public static void onMethodExit(int methodId, int bbVector) {
    sBasicBlockStats[methodId] |= bbVector;
  }

  public static void onMethodExit(int methodId, short bbVector1, short bbVector2) {
    sBasicBlockStats[methodId] |= bbVector1;
    sBasicBlockStats[methodId + 1] |= bbVector2;
  }

  public static void onMethodExit(
      int methodId, short bbVector1, short bbVector2, short bbVector3) {
    sBasicBlockStats[methodId] |= bbVector1;
    sBasicBlockStats[methodId + 1] |= bbVector2;
    sBasicBlockStats[methodId + 2] |= bbVector3;
  }

  public static void onMethodExit(
      int methodId, short bbVector1, short bbVector2, short bbVector3, short bbVector4) {
    sBasicBlockStats[methodId] |= bbVector1;
    sBasicBlockStats[methodId + 1] |= bbVector2;
    sBasicBlockStats[methodId + 2] |= bbVector3;
    sBasicBlockStats[methodId + 3] |= bbVector4;
  }

  public static void onMethodExit(
      int methodId,
      short bbVector1,
      short bbVector2,
      short bbVector3,
      short bbVector4,
      short bbVector5) {
    sBasicBlockStats[methodId] |= bbVector1;
    sBasicBlockStats[methodId + 1] |= bbVector2;
    sBasicBlockStats[methodId + 2] |= bbVector3;
    sBasicBlockStats[methodId + 3] |= bbVector4;
    sBasicBlockStats[methodId + 4] |= bbVector5;
  }

  public static void onMethodExit(int methodId, short[] bbVector) {
    for (int i = 0; i < bbVector.length; i++) {
      sBasicBlockStats[methodId + 1] |= bbVector[i];
    }
  }
}
