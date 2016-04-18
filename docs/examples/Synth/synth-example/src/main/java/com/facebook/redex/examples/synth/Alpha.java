/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.examples.synth;

public class Alpha {

    private static int alpha;

    public Alpha(int initialValue) {
        alpha = initialValue;
    }

    public class Beta {
        public int doubleAlpha() {
            return 2 * alpha;
        }
    }
}
