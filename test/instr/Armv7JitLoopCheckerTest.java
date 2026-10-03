/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

// The shape of a generated JSON parser: each case of the loop writes its own
// local, and every local is read after the loop. It must outscore every other
// method in the test APKs, which include test-runner libraries.
public class Armv7JitLoopCheckerTest {
  public static int parse(int[] tokens) {
    int f0 = 0;
    int f1 = 0;
    int f2 = 0;
    int f3 = 0;
    int f4 = 0;
    int f5 = 0;
    int f6 = 0;
    int f7 = 0;
    int f8 = 0;
    int f9 = 0;
    int f10 = 0;
    int f11 = 0;
    int f12 = 0;
    int f13 = 0;
    int f14 = 0;
    int f15 = 0;
    int f16 = 0;
    int f17 = 0;
    int f18 = 0;
    int f19 = 0;
    int f20 = 0;
    int f21 = 0;
    int f22 = 0;
    int f23 = 0;
    int f24 = 0;
    int f25 = 0;
    int f26 = 0;
    int f27 = 0;
    int f28 = 0;
    int f29 = 0;
    int f30 = 0;
    int f31 = 0;
    int f32 = 0;
    int f33 = 0;
    int f34 = 0;
    int f35 = 0;
    int f36 = 0;
    int f37 = 0;
    int f38 = 0;
    int f39 = 0;
    int f40 = 0;
    int f41 = 0;
    int f42 = 0;
    int f43 = 0;
    int f44 = 0;
    int f45 = 0;
    int f46 = 0;
    int f47 = 0;
    int f48 = 0;
    int f49 = 0;
    int f50 = 0;
    int f51 = 0;
    int f52 = 0;
    int f53 = 0;
    int f54 = 0;
    for (int token : tokens) {
      switch (token) {
        case 0:
          f0 = token;
          break;
        case 1:
          f1 = token;
          break;
        case 2:
          f2 = token;
          break;
        case 3:
          f3 = token;
          break;
        case 4:
          f4 = token;
          break;
        case 5:
          f5 = token;
          break;
        case 6:
          f6 = token;
          break;
        case 7:
          f7 = token;
          break;
        case 8:
          f8 = token;
          break;
        case 9:
          f9 = token;
          break;
        case 10:
          f10 = token;
          break;
        case 11:
          f11 = token;
          break;
        case 12:
          f12 = token;
          break;
        case 13:
          f13 = token;
          break;
        case 14:
          f14 = token;
          break;
        case 15:
          f15 = token;
          break;
        case 16:
          f16 = token;
          break;
        case 17:
          f17 = token;
          break;
        case 18:
          f18 = token;
          break;
        case 19:
          f19 = token;
          break;
        case 20:
          f20 = token;
          break;
        case 21:
          f21 = token;
          break;
        case 22:
          f22 = token;
          break;
        case 23:
          f23 = token;
          break;
        case 24:
          f24 = token;
          break;
        case 25:
          f25 = token;
          break;
        case 26:
          f26 = token;
          break;
        case 27:
          f27 = token;
          break;
        case 28:
          f28 = token;
          break;
        case 29:
          f29 = token;
          break;
        case 30:
          f30 = token;
          break;
        case 31:
          f31 = token;
          break;
        case 32:
          f32 = token;
          break;
        case 33:
          f33 = token;
          break;
        case 34:
          f34 = token;
          break;
        case 35:
          f35 = token;
          break;
        case 36:
          f36 = token;
          break;
        case 37:
          f37 = token;
          break;
        case 38:
          f38 = token;
          break;
        case 39:
          f39 = token;
          break;
        case 40:
          f40 = token;
          break;
        case 41:
          f41 = token;
          break;
        case 42:
          f42 = token;
          break;
        case 43:
          f43 = token;
          break;
        case 44:
          f44 = token;
          break;
        case 45:
          f45 = token;
          break;
        case 46:
          f46 = token;
          break;
        case 47:
          f47 = token;
          break;
        case 48:
          f48 = token;
          break;
        case 49:
          f49 = token;
          break;
        case 50:
          f50 = token;
          break;
        case 51:
          f51 = token;
          break;
        case 52:
          f52 = token;
          break;
        case 53:
          f53 = token;
          break;
        case 54:
          f54 = token;
          break;
        default:
          skip(token);
      }
    }
    return f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9 + f10 + f11 + f12 +
        f13 + f14 + f15 + f16 + f17 + f18 + f19 + f20 + f21 + f22 + f23 + f24 +
        f25 + f26 + f27 + f28 + f29 + f30 + f31 + f32 + f33 + f34 + f35 + f36 +
        f37 + f38 + f39 + f40 + f41 + f42 + f43 + f44 + f45 + f46 + f47 + f48 +
        f49 + f50 + f51 + f52 + f53 + f54;
  }

  private static void skip(int token) {
    if (token < 0) {
      throw new IllegalArgumentException();
    }
  }
}
