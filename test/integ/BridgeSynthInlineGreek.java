/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;
class Alpha {

  public Alpha(int v) {
    x = v;
  }

  private static int x;

  public class Beta {
    public int doublex() {
      return 2 * x;
    }
  }
}



/*
This example below is designed to make sure we do not remove synthetic
methods that refer to non-concrete fields. The invoke-static that
arises from the use of this.out creates a reference to a field that
is defined in a different compilation unit (java.io.FilterWriter):

0004fc: 5410 0200                              |0000: iget-object v0, v1, Lcom/facebook/redextest/Gamma$Delta;.this$0:Lcom/facebook/redextest/Gamma; // field@0002
000500: 7110 0b00 0000                         |0002: invoke-static {v0}, Lcom/facebook/redextest/Gamma;.access$000:(Lcom/facebook/redextest/Gamma;)Ljava/io/Writer; // method@000b
000506: 0c00                                   |0005: move-result-object v0
000508: 1100                                   |0006: return-object v0


We must not optimize this synthetic getter. If we do we get a bad access to
a protected field when getWriter is called:

invoke-virtual {v2}, Lcom/facebook/redextest/Gamma$Delta;.getWriter:()Ljava/io/Writer; // method@0008

*/

class Gamma extends java.io.FilterWriter {

  public Gamma(java.io.Writer writer) {
    super(writer);
  }

  class Delta {
    public java.io.Writer getWriter() {
      return out;
    }
  }
}

/*
The class SyntheticConstructor is added to test whether the const/4 insn
before calling a synthetic constructor can be removed by LocalDcePass
*/

class SyntheticConstructor {
  public SyntheticConstructor(int i) {
    x = i;
  }
  private static int x;
  private SyntheticConstructor() {
    x = 1;
  }
  public class InnerClass {
    private SyntheticConstructor sc;
    public InnerClass() {
      sc = new SyntheticConstructor();
    }
    public int getValue() {
      return sc.x;
    }
  }
}
