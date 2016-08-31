/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

import java.util.List;

public class Delta {

  public static int alpha;
  private static int beta;
  private final int gamma = 48;

  public Delta() {}
  public Delta(int i) {}

  public class A {}

  public class B {}

  public class C {
    int i;
    public int iValue() { return i; }
  }

  public class D {
    int i;
    public int iValue() { return i; }
  }

  public class E {
    int i;
    public int iValue() { return i; }
  }

  public class F {
    int wombat;
    final int numbat = 42;
    public int numbatValue() { return numbat; }
  }

  public class G {
    int wombat;
    public int wombatValue() { return wombat; }
  }

  public class H {
    int wombat ;
    boolean numbat;
    public int myIntValue() { return wombat; }
    public boolean myBoolValue() { return numbat; }
  }

  public class I {
    int wombat;
    int wombat_alpha;
    int numbat;
  }

  // Keep rule rule will match black_bear but not brown_bear because
  // $$ will not match against primitive types.
  // -keep class com.facebook.redex.test.proguard.Delta$J {
  //  ** *_bear;
  //  public *** alpha?;
  //  public ** beta*;
  // }
  public class J {
     public int brown_bear; // not kept, primitive type
     public String black_bear; // kept, class type
     public int[] grizzly_bear; // not kept, array type
     public String[] polar_bear; // not kept, array type
     public int alpha0; // kept by *** alpha?
     public int[] alpha1; // kept by *** alpha?
     public int[][] alpha2; // kept by *** alpha?
     public int beta0; // not kept, primitive type
     public List<String> beta; // kept, class type
     public List<Integer>[] beta1; // not kept, array type
  }
}
