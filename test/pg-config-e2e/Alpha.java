/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

public class Alpha {

    private int wombat;

    public Alpha () {
        wombat = 18;
    }

    public int doubleWombat() {
        return 2 * wombat;
    }
}
