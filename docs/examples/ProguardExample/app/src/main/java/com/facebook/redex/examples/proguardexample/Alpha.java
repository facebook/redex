/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.examples.proguardexample;

public class Alpha implements Greek {

    private int wombat;

    public Alpha () {
        wombat = 21;
    }

    public int doubleWombat() {
        return 2 * wombat;
    }
}
