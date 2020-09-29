/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

public class ABExperimentContextTest {
  public static int basicMethod() {
    return 5;
  }

  public int getNum() {
    return getSixPrivate();
  }

  private int getSixPrivate() {
    return 6;
  }
}
