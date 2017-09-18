/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

public class Iota {

  public interface MySerializable {
    public int encode(int originalVal);
    public int decode(int encodedVal);
  }

  // Direct use of Alpha via keep.
  public class Alpha implements MySerializable {
    public int encode(int originalVal) { return originalVal + 1; };
    public int decode(int encodedVal) { return encodedVal - 1; };
    public int wombat() { return 42; }
  }

  // No keep on Beta, so pruned.
  public class Beta implements MySerializable {
    public int encode(int originalVal) { return originalVal + 2; };
    public int decode(int encodedVal) { return encodedVal - 2; };
    public int numbat() { return 1066; }
  }

  // Indirect use of Gamma via SomeOther, so not deleted.
  public class Gamma implements MySerializable {
    public int encode(int originalVal) { return originalVal + 3; };
    public int decode(int encodedVal) { return encodedVal - 3; };
    public int numbat1() { return 1066; }
  }

  public class SomeOther {
     public Gamma gamma;
     public SomeOther() {
       gamma = new Gamma();
     }
  }
}
