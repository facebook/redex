/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

class Type1 {
}

class Type2 {
}

class Type3 {
  class Type4 {
  }
}

// Type5 is never referenced as string literal into Class.forName
class Type5 {
}

public class ReachableClasses {
    public static void foo() throws Exception {
      Class cls;
      // Types 2-4 will be found as reachable...
      cls = Class.forName("com.facebook.redextest.Type2");
      cls = Class.forName("com.facebook.redextest.Type3");
      cls = Class.forName("com.facebook.redextest.Type3$Type4");
      // Type 1 and Type5 will not be found as reachable.
      // It'd be nice if Type1 was found as reachable, but not yet.
      cls = Class.forName(Type1.class.getName());
      cls = Class.forName("com.facebook.redextest.Type5Foo");
      cls = Class.forName("com.facebook.redextest.Type5"+"Foo");
      cls = Class.forName("com.facebook.redextest.Type5$Bar");
      cls = Class.forName("com.facebook.redextest.Type5"+"$Bar");
    }
}
