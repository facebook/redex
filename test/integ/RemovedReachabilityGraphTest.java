/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

// The only keep-rule entry point. Everything reachable from here survives.
public class RemovedReachabilityGraphTest {
  public void entry() {
    new Kept().kept();
  }
}

class Kept {
  public void kept() {}

  // Nothing calls this, so the member is removed while its owner is not.
  public void unusedMember() {}
}

// Wholly removed, together with everything it declares.
class DeadOwner {
  void run() {
    DeadTarget.staticMethod();
  }
}

class DeadTarget {
  static void staticMethod() {}
}

// Redex retains annotation classes, so DeadAnno itself is never removed. What
// the removed graph has to see is the class its element value names.
@Retention(RetentionPolicy.CLASS)
@interface DeadAnno {
  Class<?> value();
}

@DeadAnno(DeadViaAnno.class)
class DeadAnnotated {}

class DeadViaAnno {}
