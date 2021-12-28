/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

import android.app.Activity;
import android.graphics.Color;
import android.text.SpannableStringBuilder;

import java.util.List;

public class Delta {

    public static int alpha;
    private static int beta;
    private int gamma;
    private int gamma1;

    public static boolean sigma = true;

    public Delta() {
    }

    public Delta(int i) {
    }

    public Delta(String s) {
    }

    public class A {
    }

    public class B {
    }

    public class C {
        int i;

        C() {
        }

        C(int i) {
        }

        public int iValue() {
            return i;
        }
    }

    public class D {
        int i;

        public int iValue() {
            return i;
        }
    }

    public class E {
        int i;

        public int iValue() {
            return 42;
        }
    }

    public class F {
        int wombat;
        final int numbat = 42;

        public int numbatValue() {
            return numbat;
        }
    }

    public class G {
        int fuzzyWombat;

        public int fuzzyWombatValue() {
            return fuzzyWombat;
        }
    }

    public class H {
        int wombat;
        boolean numbat;

        public int myIntValue() {
            return wombat;
        }

        public boolean myBoolValue() {
            return numbat;
        }
    }

    public class I {
        int wombat;
        int wombat_alpha;
        int numbat;
    }

    // Keep rule rule will match black_bear but not brown_bear because
    // $$ will not match against primitive types.
    // -keep class com.facebook.redex.test.proguard.Delta$J {
    //  ** *_bear;
    //  public *** alpha?;
    //  public ** beta*;
    //  public **[] gamma*;
    // }
    public class J {
        public J() {
        }

        public J(int i) {
        }

        public J(String s) {
        }

        public int brown_bear; // not kept, primitive type
        public String black_bear; // kept, class type
        public int[] grizzly_bear; // not kept, array type
        public String[] polar_bear; // not kept, array type
        public int alpha0; // kept by *** alpha?
        public int[] alpha1; // kept by *** alpha?
        public int[][] alpha2; // kept by *** alpha?

        public void alpha3() {
        }

        public int beta0; // not kept, primitive type
        public List<String> beta; // kept, class type
        public List<Integer>[] beta1; // not kept, array type
        public int[] gamma1; // not kept because ** does not match primtivie int
        public String[] gamma2; // kept because ** matches class and [] matches array

        public int omega(int int_arg, boolean bool_arg, String string_arg, char char_arg) {
            return int_arg + string_arg.length();
        }

        public int omega(short s) {
            return 42;
        }

        ;

        public int omega(String s) {
            return s.length();
        } // No keep rule, so pruned.

        public int omega(int hastings) {
            return 1066;
        } // Kept by the rule  public int omega(%);

        // All thetas kept by (...)
        public int theta(int int_arg, boolean bool_arg, String string_arg, char char_arg) {
            return int_arg + string_arg.length();
        }

        public int theta(short s) {
            return 42;
        }

        ;

        public int theta(String s) {
            return s.length();
        }

        // No iotas should be matched.
        public int iota(int int_arg, boolean bool_arg, String string_arg, char char_arg) {
            return int_arg + string_arg.length();
        }

        public int iota(short s) {
            return 42;
        }

        ;

        public int iota(short[] s) {
            return 42;
        }

        ;

        public int iota(String s) {
            return s.length();
        }

        public void zeta0() {
        }

        public String zeta1() {
            return "and everything";
        }
    }

    @DoNotStrip
    public class K {
        public int alpha;
        @DoNotStrip
        public int beta;

        public void gamma() {
        }

        @DoNotStrip
        public void omega() {
        }
    }

    public class L {
        public void alpha0() {
        }

        protected void alpha1() {
        }

        private void alpha2() {
        }

        public void beta0() {
        }

        protected void beta1() {
        }

        private void beta2() {
        }

        public void gamma0() {
        }

        protected void gamma1() {
        }

        private void gamma2() {
        }
    }

    public class M extends Epsilon {
    }

    public class N extends M {
    }

    public final class O extends Epsilon {
    }

    public class P extends G {
    }

    public class Q1 extends android.graphics.Color {
    }

    public class Q2 extends SpannableStringBuilder {
    }

    @DoNotStrip
    public class R {
    }

    @DoNotStrip
    public class R0 {
    }

    public class R1 {
    }

    public class S0 extends R0 {
    }

    public class S1 extends R1 {
    }

    public interface S3 {
    }

    public interface S4 extends S3 {
    }

    public interface S5 extends S4 {
    }

    public class S6 implements S4, S5 {
    }

    public class T1 implements S3 {
        public int alpha;
        public int beta;
        public int gamma;
    }

    public class T2 extends T1 {
    }

    public class U {
        int i = 0;

        void logger() {
        }

        void mutator() {
            i = i + 1;
            logger();
        }
    }

    public class VT {
    }

    public class V {
        VT goat;
        VT sheep;
        int lama;
    }

    public class W {
        int haskell;
        boolean scala;
    }

    public class X {

        public class X1 {
        }

        public class X2 {
            private int x;

            public X2(int i) {
                x = i;
            }
        }
    }

    class E0 {
    }

    class E1 extends E0 {
        int shark() {
            return 3;
        }

    }

    class E2 extends E1 {
        int crab;
    }

    class E3 extends E2 {
        int seahorse;
    }

    class E4 extends E3 {
        int tuna1() {
            return 4;
        }

    }

    class E5 extends E4 {
        int goldfish() {
            return 6;
        }

        int shark() {
            return 5;
        }

        int tuna1() {
            return 7;
        }

        int tuna2() {
            return 11;
        }
    }

    class E6 extends E5 {
    }

    class E7 extends E6 {
        int crab;
        int seahorse;
        int octopus;

        int shark() {
            return 51;
        }

        int tuna1() {
            return 71;
        }

        int tuna2() {
            return 171;
        }
    }

}
