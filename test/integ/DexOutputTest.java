/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

public class DexOutputTest {
  public static int AjustReturnFive() {
    return 5;
  }

  public int BjustCallSixpublic() {
    return DgetSixpublic();
  }

  public int CsomeLogic(int y) {
    int x = 0;
    x = x / y;
    x++;
    return x;
  }

  public int DgetSixpublic() {
    return 6;
  }

  public static int EjustReturnFive() {
    return 5;
  }

  public int FsomeLogic(int y) {
    int x = 0;
    x = x / y;
    x++;
    return x;
  }

  public int GjustCallSixpublic() {
    return DgetSixpublic();
  }

  public int HsomeLogic(int y) {
    int x = 0;
    x = x / y;
    x++;
    return x;
  }

  class PerfSensitiveClass {
    public int FsomeLogic(int y) {
      int x = 0;
      x = x / y;
      x++;
      return x;
    }

    public int EjustReturnFive() {
      return 5;
    }
  }

  class NonPerfSensitiveClass {
    public int FsomeLogic(int y) {
      int x = 0;
      x = x / y;
      x++;
      return x;
    }

    public int EjustReturnFive() {
      return 5;
    }
  }

  class SecondPerfSensitiveClass {
    public int FsomeLogic(int y) {
      int x = 0;
      x = x / y;
      x++;
      return x;
    }

    public int EjustReturnFive() {
      return 5;
    }
  }
}
