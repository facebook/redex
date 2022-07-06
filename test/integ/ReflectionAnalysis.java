/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

 package com.facebook.redextest;

 import java.lang.reflect.*;

 class ReflectionAnalysis {
   static class Isolate {
     public int foo1;
     public double foo2;

     public int moo1() { return foo1; }
     public double moo2() { return foo2; }

     private String getName() {
       return "foo1";
     }

     private static void check(String x, String y) {}

     public static void main(String[] args) {
       Isolate p = new Isolate();
       try {
         java.lang.Class isolate_class = Isolate.class;
         java.lang.Class isolate_class2 = p.getClass();
         Field f1 = Isolate.class.getField("foo1");
         Field f2 = isolate_class.getDeclaredField("foo2");
         Method m1 = isolate_class.getMethod("moo1");
         Method m2 = isolate_class2.getDeclaredMethod("moo2");
         check("f1", f1.getName());
         check("f2", f2.getName());
         check("m1", m1.getName());
         check("m2", m2.getName());

         Field f3 = Isolate.class.getField(p.getName());
         check("f3", f3.getName());

         String name = "foo2";
         String name2 = name;
         Field f4 = isolate_class.getField(name);
         Field f5 = isolate_class2.getField(name2);
         check("f4", f4.getName());
         check("f5", f5.getName());

         StringBuilder nameBase = new StringBuilder("moo");
         Field f6 = isolate_class.getField(nameBase + "1");
         Method m7 = isolate_class.getMethod(nameBase + "2");
         check("f6", f6.getName());
         check("m7", m7.getName());

         Field f8;
         if (args.length > 2) {
           f8 = isolate_class.getField("foo1");
         } else {
           f8 = isolate_class.getField("foo2");
         }
         check("f8", f8.getName());

         java.lang.Class isolate_class3 = java.lang.Class.forName(
             "com.facebook.redextest.ReflectionAnalysis.Isolate");
         Field f9 = isolate_class3.getField("foo1");
         check("f9", f9.getName());
       } catch (Exception e) {
         System.out.println(e.toString());
       }
     }
   }
 }
