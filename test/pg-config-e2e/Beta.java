/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
