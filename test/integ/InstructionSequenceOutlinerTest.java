/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class InstructionSequenceOutlinerTest {
    static void distraction1() {}
    static void distraction2() {}
    static void distraction3() {}
    static void println(String s, String t, String u) {}

    public void basic1() {
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
    }

    public void basic2() {
      distraction1();
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
    }

    public void basic3() {
      distraction1();
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
      distraction2();
    }

    public void basic4() {
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
      distraction2();
    }

    public void in_try() {
      try {
        println("a", "b", "c");
        println("d", "e", "f");
        println("g", "h", "i");
        println("j", "k", "l");
        println("m", "n", "o");
      } catch (Exception e) {}
    }

    public void in_try_ineligible_due_to_different_catches() {
      try {
        println("a", "b", "c");
        println("d", "e", "f");
        println("g", "h", "i");
      } catch (Exception e) {}
      try {
        println("j", "k", "l");
        println("m", "n", "o");
      } catch (Exception e) {}
    }

    public void in_try_ineligible_due_to_conditional_branch(boolean b) {
      try {
        println("a", "b", "c");
        println("d", "e", "f");
        println("g", "h", "i");
        if (b) return;
        println("j", "k", "l");
        println("m", "n", "o");
      } catch (Exception e) {}
    }

    public void twice1() {
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");

      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
    }

    public void twice2() {
      distraction1();
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
      distraction2();
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", "h", "i");
      println("j", "k", "l");
      println("m", "n", "o");
      distraction3();
    }

    public void param1() {
      String s = "param1";
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", s, "i");
      println("j", "k", "l");
      println("m", "n", "o");
    }

    public void param2() {
      String s = "param2";
      println("a", "b", "c");
      println("d", "e", "f");
      println("g", s, "i");
      println("j", "k", "l");
      println("m", "n", "o");
    }

    public int result1() {
      println("a", "b", "c");
      int x = 1;
      println("d", "e", "f");
      x++;
      println("g", "h", "i");
      x++;
      println("j", "k", "l");
      x++;
      println("m", "n", "o");
      // outlining ends here
      x += 27;
      return x;
    }

    public int result2() {
      println("a", "b", "c");
      int x = 1;
      println("d", "e", "f");
      x++;
      println("g", "h", "i");
      x++;
      println("j", "k", "l");
      x++;
      println("m", "n", "o");
      // outlining ends here
      x += 77;
      return x;
    }

    public int normalization1(int x) {
      // outlining starts here
      x+=1;
      x+=2;
      x+=3;
      x+=4;
      x+=5;
      x+=6;
      x+=7;
      x+=8;
      x+=9;
      x+=10;
      x+=11;
      x+=12;
      x+=13;
      x+=14;
      x+=15;
      x+=16;
      x+=17;
      x+=18;
      return x /* outlining ends here */;
    }

    public int normalization2(int x) {
      int extra_var_shifts_all_registers = 0;
      // outlining starts here
      x+=1;
      x+=2;
      x+=3;
      x+=4;
      x+=5;
      x+=6;
      x+=7;
      x+=8;
      x+=9;
      x+=10;
      x+=11;
      x+=12;
      x+=13;
      x+=14;
      x+=15;
      x+=16;
      x+=17;
      x+=18;
      return x + /* outlining ends here */ extra_var_shifts_all_registers;
    }

    public int defined_reg_escapes_to_catch(int x) {
      try {
        // Something that can actually throw
        println("a", "b", "c");
        // outlining potentially starts here
        x+=1;
        x+=2;
        x+=3;
        x+=4;
        x+=5;
        x+=6;
        x+=7;
        x+=8;
        x+=9;
        x+=10;
        x+=11;
        x+=12;
        x+=13;
        x+=14;
        x+=15;
        x+=16;
        x+=17;
        x+=18;
        // outlining potentially ends here
        // Something that can actually throw
        println("a", "b", "c");
        return x;
      } catch (Exception e) {
        return x;
      }
    }

    public int big_block_can_end_with_no_tries1(int x) {
      try {
        // outlining potentially starts here
        println("big_block_can_end_with_no_tries", "b", "c");
        x+=1;
        x+=2;
        x+=3;
        x+=4;
        x+=5;
        x+=6;
        x+=7;
        x+=8;
        x+=9;
        x+=10;
        x+=11;
        x+=12;
        x+=13;
        x+=14;
        x+=15;
        x+=16;
        x+=17;
        x+=18;
        return /* outlining ends here */ x;
      } catch (Exception e) {
        return 1;
      }
    }

    public int big_block_can_end_with_no_tries2(int x) {
      try {
        // outlining potentially starts here
        println("big_block_can_end_with_no_tries", "b", "c");
        x+=1;
        x+=2;
        x+=3;
        x+=4;
        x+=5;
        x+=6;
        x+=7;
        x+=8;
        x+=9;
        x+=10;
        x+=11;
        x+=12;
        x+=13;
        x+=14;
        x+=15;
        x+=16;
        x+=17;
        x+=18;
        return /* outlining ends here */ x;
      } catch (Exception e) {
        return 2;
      }
    }

    public int two_out_regs(int x, int y) {
      x += y; y += x;
      x += y; y += x;
      x += y; y += x;
      x += y; y += x;
      int a = x + y;

      x += y; y += x;
      x += y; y += x;
      x += y; y += x;
      x += y; y += x;
      return a + x + y;
    }

    public void type_demand1() {
      Object s = "Hello";
      // outlining starts here
      String t = (String) s;
      println(t, "a", "b");
      println(t, "c", "d");
      println(t, "e", "f");
      println(t, "g", "h");
      println(t, "i", "j");
      println(t, "k", "l");
    }

    public void type_demand2() {
      Object s = "World";
      // outlining starts here
      String t = (String) s;
      println(t, "a", "b");
      println(t, "c", "d");
      println(t, "e", "f");
      println(t, "g", "h");
      println(t, "i", "j");
      println(t, "k", "l");
    }

    static class Nested1 {
      public static void distributed() {
        println("1", "2", "3");
        println("2", "3", "4");
        println("3", "4", "5");
        println("4", "5", "6");
        println("5", "6", "7");
      }
    }

    static class Nested2 {
      public static void distributed() {
        println("1", "2", "3");
        println("2", "3", "4");
        println("3", "4", "5");
        println("4", "5", "6");
        println("5", "6", "7");
      }
    }
}
