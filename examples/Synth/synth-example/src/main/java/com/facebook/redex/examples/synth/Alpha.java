/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
