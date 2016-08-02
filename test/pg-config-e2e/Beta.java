// Copyright 2004-present Facebook. All Rights Reserved.

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
