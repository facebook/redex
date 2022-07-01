/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package dex.test;

import java.util.ArrayList;
import java.util.List;

public class TestClass {
  // Ensure we have an extensive list of string literals for `find_string_idx`.
  public List<String> generateStrings() {
    List<String> ret = new ArrayList<>();

    ret.add("");
    ret.add("A");
    ret.add("Hello");
    ret.add("World");
    ret.add("World1");
    ret.add("World2");
    ret.add("World3");
    ret.add("World4");
    ret.add("World5");
    ret.add("World6");
    ret.add("World7");
    ret.add("World8");
    ret.add("World9");
    // Some unicode strings.
    ret.add("\u0300\u0301\u0302");
    ret.add("\u0300\u0301\u0303");
    ret.add("\u0301\u0301\u0302");
    ret.add("\u0301\u0300\u0302");

    return ret;
  }
}
