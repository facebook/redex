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
