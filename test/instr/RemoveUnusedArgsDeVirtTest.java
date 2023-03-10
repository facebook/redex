/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.assertj.core.api.Assertions.*;
import android.util.Log;
import android.os.Build;
import android.annotation.TargetApi;
import com.facebook.annotations.*;
import org.junit.Test;

final class Foo {
  private int x;

  // One removable arg
  public Foo(int x) {
    this.x = 1;
  }
  public int getX(){
    return x;
  }

}

final class Bar {
  private int x;

  // One removable arg
  @DoNotDevirtualize
  public Bar(int x) {
    this.x = 3;
  }
  public int getX(){
    return x;
  }

}


public class RemoveUnusedArgsDeVirtTest {
  @Test
  public void testInvoke() {
    Foo f1 = new Foo(1);
    assertThat(f1.getX()).isEqualTo(1);
    Bar f2 = new Bar(3);
    assertThat(f2.getX()).isEqualTo(3);
  }

}

