/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.redex;

@interface Funny {

}

@interface VeryFunny {

}

@interface Zero {
  // This is a field on an annotation. Why someone would do this, I don't know.
  Funny f = null;
  VeryFunny[] a = null;
}

@interface One {
  int x() default 10;
  Zero zero();
}

@interface Two {
  int y();
  One[] one() default {};
}

@interface Unused {
  int z();
}

@Two(y = 1)
class Use {}

class Main {
    public static void go() {
        System.out.println(Two.class);
    }
}
