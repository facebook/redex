/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;

/**
 * This is a relatively simple mock up hierarchy that's close to what the
 * generated GraphQL code looks like.
 */
interface RootInterface {}

interface SuperInterface extends RootInterface {
  int getInt();
  String getStr();
  String concat(String a, String b);
  int add(int a, int b);
  String getTypeName();
}

interface BridgeInterface {
  String getStr();
}

interface AInterface extends SuperInterface {
  int getInt();
  String getStr();
  String concat(String a, String b);
  int add(int a, int b);
  String getTypeName();
}

interface BInterface extends SuperInterface, BridgeInterface {
  int getInt();
  String getStr();
  String concat(String a, String b);
  int add(int a, int b);
  String getTypeName();
}

interface UnremovableInterface extends SuperInterface {
  int getInt();
  String getStr();
  String concat(String a, String b);
  int add(int a, int b);
  String getTypeName();
}

interface RepeatABaseTypeMethod {
  int getObjectType();
}

// Dummy interface referencing class.
class Dummy {
  AInterface aInterfaceField;
}

class FirstBaseModel {
  public String getTypeName() { return "first type"; }
}

class SecondBaseModel {
  public final String getTypeName() { return "second type"; }
  public final int getObjectType() { return 42; };
}

class FirstAModel extends FirstBaseModel implements AInterface {
  @Override
  public String getTypeName() {
    return "first a type";
  }
  public int getInt() { return 42; }
  public String getStr() { return "FirstA"; }
  public String concat(String a, String b) { return a + b + "FirstA"; }
  public int add(int a, int b) { return a + b + 1; }
}

class SecondAModel
    extends SecondBaseModel implements AInterface, RepeatABaseTypeMethod {
  public int getInt() { return 43; }
  public String getStr() { return "SecondA"; }
  public String concat(String a, String b) { return a + b + "SecondA"; }
  public int add(int a, int b) { return a + b + 2; }
}

class FirstBModel extends FirstBaseModel implements BInterface {
  @Override
  public String getTypeName() {
    return "first b type";
  }
  public int getInt() { return 44; }
  public String getStr() { return "FirstB"; }
  public String concat(String a, String b) { return a + b + "FirstB"; }
  public int add(int a, int b) { return a + b + 3; }
}

class SecondBModel
    extends SecondBaseModel implements BInterface, RepeatABaseTypeMethod {
  public int getInt() { return 45; }
  public String getStr() { return "SecondB"; }
  public String concat(String a, String b) { return a + b + "SecondB"; }
  public int add(int a, int b) { return a + b + 4; }
}

class UnremovableModel extends FirstBaseModel implements UnremovableInterface {
  public int getInt() { return 46; }
  public String getStr() { return "Unremovable"; }
  public String concat(String a, String b) { return a + b + "Unremovable"; }
  public int add(int a, int b) { return a + b + 5; }
}

public class RemoveInterfaceTest {

  @Test
  public void testInvokeInterfaceSimple() {
    AInterface fam = new FirstAModel();
    assertThat(fam.getInt()).isEqualTo(42);
    assertThat(fam.getStr()).isEqualTo("FirstA");
    assertThat(fam.concat("ho", "ha!")).isEqualTo("hoha!FirstA");
    assertThat(fam.add(2, 3)).isEqualTo(6);
    assertThat(fam.getTypeName()).isEqualTo("first a type");

    AInterface sam = new SecondAModel();
    assertThat(sam.getInt()).isEqualTo(43);
    assertThat(sam.getStr()).isEqualTo("SecondA");
    assertThat(sam.concat("ho", "ha!")).isEqualTo("hoha!SecondA");
    assertThat(sam.add(3, 4)).isEqualTo(9);
    assertThat(sam.getTypeName()).isEqualTo("second type");
    RepeatABaseTypeMethod rsa = (RepeatABaseTypeMethod) sam;
    assertThat(rsa.getObjectType()).isEqualTo(42);

    BInterface fbm = new FirstBModel();
    assertThat(fbm.getInt()).isEqualTo(44);
    assertThat(fbm.getStr()).isEqualTo("FirstB");
    assertThat(fbm.concat("ho", "ha!")).isEqualTo("hoha!FirstB");
    assertThat(fbm.add(2, 3)).isEqualTo(8);
    assertThat(fbm.getTypeName()).isEqualTo("first b type");

    BInterface sbm = new SecondBModel();
    assertThat(sbm.getInt()).isEqualTo(45);
    assertThat(sbm.getStr()).isEqualTo("SecondB");
    assertThat(sbm.concat("ho", "ha!")).isEqualTo("hoha!SecondB");
    assertThat(sbm.add(3, 4)).isEqualTo(11);
    assertThat(sbm.getTypeName()).isEqualTo("second type");
    RepeatABaseTypeMethod rsb = (RepeatABaseTypeMethod) sam;
    assertThat(rsb.getObjectType()).isEqualTo(42);

    // new array
    AInterface[] arr = new AInterface[1];
    arr[0] = new FirstAModel();
    assertThat(arr[0].getInt()).isEqualTo(42);
    assertThat(arr[0].getStr()).isEqualTo("FirstA");
    assertThat(arr[0].concat("ho", "ha!")).isEqualTo("hoha!FirstA");
    assertThat(arr[0].add(2, 3)).isEqualTo(6);
    assertThat(arr[0].getTypeName()).isEqualTo("first a type");

    // Bridge interface
    BridgeInterface br = new FirstBModel();
    assertThat(br.getStr()).isEqualTo("FirstB");
  }

  @Test
  public void testUnremovableInterface() {
    UnremovableModel r = new UnremovableModel();
    assertThat(r instanceof UnremovableInterface).isTrue();
    assertThat(r.getInt()).isEqualTo(46);
    assertThat(r.getStr()).isEqualTo("Unremovable");
  }
}
