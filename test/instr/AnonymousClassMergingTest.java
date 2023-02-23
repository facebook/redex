/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This is a seemingly simple test for AnonymousClassMerging.
 * However, the pass will pick up other classes bundled in the testing apk, e.g., 'Lcom/google/..'.
 * We had to apply some filtering to narrow down the scope of the merging transformation.
 * The test covers more obscure default interface method cases such as an external default method implementation.
 */

package com.facebook.redextest;

import android.annotation.TargetApi;
import java.util.Arrays;
import java.util.Comparator;
import org.junit.Test;

import static org.fest.assertions.api.Assertions.assertThat;

interface Interface1 {
  default int magic1() { return 42; }
  int magic2();
}

public class AnonymousClassMergingTest {

  @Test
  public void testDefaultInterfaceMethod() {
    Interface1 a = new Interface1() {
      @Override
      public int magic1() {
        return 24;
        //
      }
      @Override
      public int magic2() {
        return 142;
      }
    };
    assertThat(a.magic1()).isEqualTo(24);
    assertThat(a.magic2()).isEqualTo(142);

    Interface1 b = new Interface1() {
      @Override
      public int magic2() {
        return 143;
      }
    };
    assertThat(b.magic1()).isEqualTo(42);
    assertThat(b.magic2()).isEqualTo(143);

    Interface1 c = new Interface1() {
      @Override
      public int magic2() {
        return 144;
      }
    };
    assertThat(c.magic1()).isEqualTo(42);
    assertThat(c.magic2()).isEqualTo(144);

    Interface1 d = new Interface1() {
      @Override
      public int magic2() {
        return 145;
      }
    };
    assertThat(d.magic1()).isEqualTo(42);
    assertThat(d.magic2()).isEqualTo(145);

    Interface1 e = new Interface1() {
      @Override
      public int magic2() {
        return 146;
      }
    };
    assertThat(e.magic1()).isEqualTo(42);
    assertThat(e.magic2()).isEqualTo(146);
  }

  class Item {
    Item(int v) { this.val = v; }
    public final int val;
    @Override
    public String toString() { return String.valueOf(this.val); }
  }

  @Test
  @TargetApi(24)
  public void testExternalDefaultInterfaceMethod() {
    Comparator<Item> c1 = new Comparator<Item>() {
      @Override
      public int compare(Item l, Item r) {
        return (int) (l.val - r.val);
      }
    };
    Item[] arr1 = {new Item(2), new Item(1)};
    Arrays.sort(arr1, c1);
    assertThat(Arrays.toString(arr1)).isEqualTo("[1, 2]");
    Arrays.sort(arr1, c1.reversed());
    assertThat(Arrays.toString(arr1)).isEqualTo("[2, 1]");

    Comparator<Item> c2 = new Comparator<Item>() {
      @Override
      public int compare(Item l, Item r) {
        return (int) (r.val - l.val);
      }
      @Override
      public Comparator<Item> reversed () {
        // Not really the reversed.
        return this;
      }
    };
    Item[] arr2 = {new Item(1), new Item(2)};
    Arrays.sort(arr2, c2);
    assertThat(Arrays.toString(arr2)).isEqualTo("[2, 1]");
    Arrays.sort(arr2, c2.reversed());
    assertThat(Arrays.toString(arr2)).isEqualTo("[2, 1]");
  }
}
