/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
