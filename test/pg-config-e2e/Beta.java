/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

// Beta is not used anywhere and not kept by any rules
// so it should be removed.
public class Beta {

    private int wombatBeta;

    public Beta () {
        wombatBeta = 72;
    }

    public int doubleWombatBeta() {
        return 2 * wombatBeta;
    }
}
