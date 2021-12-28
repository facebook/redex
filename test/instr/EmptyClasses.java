/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

@interface DoNotStrip{
}

// This class should be removed because it is not
// used anywhere.
public class EmptyClasses {
}

// The outer and inner classes should be removed.
class InnerEmpty {
  public class InnerClass {
  }
}

// The InnerEmpty2 class is used in the main program through
// an access to x, but the InnerClass2 is not used and should
// be removed.
class InnerEmpty2 {
  int x;
  public class InnerClass2 {
  }
}

// A class with just one instance field which is used in the main program
// so this class should not be removed.
class NotAnEmptyClass {
  int y;
}

// A class with just one static field which is used in the main program
// so this class should not be removed.
class NotAnEmptyClass2 {
  static int sy;
}

// A class with just one instance method which is used in the main
// program so this class should not be removed.
class NotAnEmptyClass3 {
  public boolean yes() { return true; }
}


// This class is extended by another class so it should not
// be removed.
abstract class NotAnEmptyClass4 {
  public abstract boolean yes();
}

// This class is used in the main program so it should not be
// removed.
class NotAnEmptyClass5 extends NotAnEmptyClass4 {
  public boolean yes() { return true; }
}

// This is an interface so it should not be removed.
interface YesNo {
  public abstract boolean yes();
  public abstract boolean no();
}

// Used in the main program so do not prune.
class MyYesNo implements YesNo {
  public boolean yes() { return true; }
  public boolean no() { return false; }
}

// Interface so do not remove.
interface Doubler {
  public int Double();
}

// Interface so do not remove.
interface Tripler {
  public int Triple();
}

// Interface so do not remove.
interface EasilyDone {}

// Ny2Or3 extends an empty interface and is used is used
// by MyBy2Or3 which is also used. Do not remove.
interface By2Or3 extends Doubler, Tripler, EasilyDone {}

// Used in the main program so do not delete.
class MyBy2Or3 implements By2Or3 {
  private int x;
  public MyBy2Or3(int v) { x = v; }
  public int Double() { return 2*x; }
  public int Triple() { return 3*x; }
}

// An exception which occurs in a throw and catch which are
// in code which is used. Do not delete.
class WombatException extends Exception {}
// An exception which is thrown but not caught.
class NumbatException extends Exception {}

// This class is used in the main program so do not delete.
class Wombat {

  static boolean MaybeExplode (boolean bang) throws WombatException {
    if (bang) {
      throw new WombatException();
    }
    return true;
  }

  static boolean BadWombat(boolean bang) {
      boolean b = false;
      try {
        b = MaybeExplode(bang);
      } catch (WombatException e) {}
      return b;
  }

  static void MaybeBadWombat(boolean bang) throws NumbatException {
    if (bang) {
      throw new NumbatException();
    }
  }
}

// An empty class which is extened later in a class which is used.
class EmptyButLaterExtended {}

// Used in the main program so do not delete.
class Extender extends EmptyButLaterExtended {
  int x;
}

// Interface not used anywhere but still should not be deleted because
// it is an interface.
interface NotUsedHere {}

// This class will be stripped because the integraton test does
// not use a ProGuard config file to express the intent of @DoNotStrip
@DoNotStrip
class DontKillMeNow {}
