/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

public final class IODIConstants {
  // These have to stay in sync with constants in `DexOutput.h`!

  public final static int IODI_LAYER_BITS = 4;
  public final static int IODI_LAYER_BOUND = 1 << (IODI_LAYER_BITS - 1);
  public final static int IODI_LAYER_SHIFT = 32 - IODI_LAYER_BITS;
  public final static int IODI_DATA_MASK = (1 << IODI_LAYER_SHIFT) - 1;
  public final static int IODI_LAYER_MASK = ((1 << IODI_LAYER_BITS) - 1)
                                             << IODI_LAYER_SHIFT;

  public static int getLayer(int line) {
    return (line & IODI_LAYER_MASK) >> IODI_LAYER_SHIFT;
  }

  public static int getEncodedLine(int line) {
    return line & IODI_DATA_MASK;
  }
}
