/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
      x+=-1;
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
      x+=-1;
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
        x+=-1;
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
        x+=-1;
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
        x+=-1;
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

    static int A;
    static Object B;
    static byte C;
    static short D;
    static char E;
    public void cfg_tree1() {
      int a = A;
      Object b = B;
      byte c = C;
      short d = D;
      char e = E;
      int un_outlinable_unique_junk = 123;
      // We'd like the outline the following.
      // The following code is shared between cfg_tree1 and cfg_tree2.
      // However, the internal "type demands" don't give enough information
      // about permissible argument types for a/b/c/d/e of an outlined
      // method, as the bytecode instruction IF_EQ could be used on floats
      // or objects as well. Special treatment of the IF-instuctions with
      // type inference helps.
      if (a == 1) {
        if (b == null) {
          if (c == 3) {
            if (d == 4) {
              if (e == 5) {
                println("1", "2", "3");
                return ;
              } else {
                println("2", "3", "4");
                return;
              }
            } else {
              println("3", "4", "5");
              return;
            }
          } else {
            return;
          }
        } else {
          return;
        }
      } else {
        return;
      }
    }

    public void cfg_tree2() {
      int a = A;
      Object b = B;
      byte c = C;
      short d = D;
      char e = E;
      int un_outlinable_unique_junk = 456;
      // We'd like the outline the following.
      if (a == 1) {
        if (b == null) {
          if (c == 3) {
            if (d == 4) {
              if (e == 5) {
                println("1", "2", "3");
                return ;
              } else {
                println("2", "3", "4");
                return;
              }
            } else {
              println("3", "4", "5");
              return;
            }
          } else {
            return;
          }
        } else {
          return;
        }
      } else {
        return;
      }
    }

    public void switch1() {
      int x = 100;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return;
        case 2:
          println("2", "3", "4");
          return;
        case 3:
          println("3", "4", "5");
          return;
        case 4:
          println("4", "5", "6");
          return;
      }
    }

    public void switch2() {
      int x = 200;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return;
        case 2:
          println("2", "3", "4");
          return;
        case 3:
          println("3", "4", "5");
          return;
        case 4:
          println("4", "5", "6");
          return;
      }
    }

    public int cfg_with_arg_and_res1(int y) {
      int x = 100;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return y + 1;
        case 2:
          println("2", "3", "4");
          return y + 2;
        case 3:
          println("3", "4", "5");
          return y + 3;
      }
      return y + 5;
    }

    public int cfg_with_arg_and_res2(int y) {
      int x = 200;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return y + 1;
        case 2:
          println("2", "3", "4");
          return y + 2;
        case 3:
          println("3", "4", "5");
          return y + 3;
      }
      return y + 5;
    }

    public int cfg_with_const_res1() {
      int x = 100;
      // Outlined code that returns constants is problematic, as the definition
      // of the constants doesn't tell us their types. Thus, we employ a special
      // analysis that looks at how the constants are later used to figure out
      // type constraints.
      switch (x) {
        case 1:
          println("1", "2", "3");
          return 0x3f800000; // floating-point 1.0
        case 2:
          println("2", "3", "4");
          return 0x40000000; // floating-point 2.0
        case 3:
          println("3", "4", "5");
          return 0x40400000; // floating-point 3.0
      }
      return 0x40800000; // floating-point 4.0
    }

    public int cfg_with_const_res2() {
      int x = 200;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return 0x3f800000; // floating-point 1.0
        case 2:
          println("2", "3", "4");
          return 0x40000000; // floating-point 2.0
        case 3:
          println("3", "4", "5");
          return 0x40400000; // floating-point 3.0
      }
      return 0x40800000; // floating-point 4.0
    }

    public float cfg_with_float_const_res1() {
      int x = 300;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return 1.0f;
        case 2:
          println("2", "3", "4");
          return 2.0f;
        case 3:
          println("3", "4", "5");
          return 3.0f;
      }
      return 4.0f;
    }

    public float cfg_with_float_const_res2() {
      int x = 400;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return 1.0f;
        case 2:
          println("2", "3", "4");
          return 2.0f;
        case 3:
          println("3", "4", "5");
          return 3.0f;
      }
      return 4.0f;
    }

    static class Base {}
    static class Sub1 extends Base {}
    static class Sub2 extends Base {}
    static class Sub3 extends Sub1 {}
    static class Sub4 extends Sub1 {}

    static void demandBase(Base base){}
    static void demandSub1(Sub1 sub1){}
    static void demandSub2(Sub2 sub2){}

    public Object cfg_with_object_res1() {
      int x = 500;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return new Sub1();
        case 2:
          println("2", "3", "4");
          return new Sub2();
        case 3:
          println("3", "4", "5");
          return new Sub3();
      }
      return new Base();
    }

    public Object cfg_with_object_res2() {
      int x = 600;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return new Sub1();
        case 2:
          println("2", "3", "4");
          return new Sub2();
        case 3:
          println("3", "4", "5");
          return new Sub3();
      }
      return new Base();
    }

    public Object cfg_with_joinable_object_res1() {
      int x = 500;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return new Sub1();
        case 2:
          println("2", "3", "4");
          return new Sub2();
        case 3:
          println("3", "4", "5");
          return new Sub3();
      }
      return null;
    }

    public Object cfg_with_joinable_object_res2() {
      int x = 600;
      switch (x) {
        case 1:
          println("1", "2", "3");
          return new Sub1();
        case 2:
          println("2", "3", "4");
          return new Sub2();
        case 3:
          println("3", "4", "5");
          return new Sub3();
      }
      return null;
    }

    public void cfg_with_object_arg1() {
      int x = 500;
      Sub3 s = new Sub3();
      switch (x) {
        case 1:
          println("1", "2", "3");
          demandBase(s);
          break;
        case 2:
          println("2", "3", "4");
          demandSub1(s);
          break;
        case 3:
          println("3", "4", "5");
          demandBase(s);
          break;
        case 4:
          println("4", "5", "6");
          demandSub1(s);
          break;
      }
    }

    public void cfg_with_object_arg2() {
      int x = 500;
      Sub4 s = new Sub4();
      switch (x) {
        case 1:
          println("1", "2", "3");
          demandBase(s);
          break;
        case 2:
          println("2", "3", "4");
          demandSub1(s);
          break;
        case 3:
          println("3", "4", "5");
          demandBase(s);
          break;
        case 4:
          println("4", "5", "6");
          demandSub1(s);
          break;
      }
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

  static class Nested3 {
    public int I;

    public void colocate_with_refs(Nested3 nested) {
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
      nested.I = 42;
    }
  }

  public void colocate_with_refs(Nested3 nested) {
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
    nested.I = 42;
  }
}
