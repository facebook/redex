/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class Data {
}

class DataHolder {
  public Data m_data;
  DataHolder(Data data) { m_data = data; }
  public Data get_data() { return m_data; }
}

class LegacyInstantiable {}

class StringInstantiable {}

class Base {
  public void foo() {}
}
class Intermediate extends Base{
  public void foo() {}
}
class RegularInstantiable extends Intermediate {
  public void foo() {} // overrides
}

class SurpriseBase {
  public void foo() {}
}
abstract class Surprise extends SurpriseBase {
  public abstract void foo();
}
class SurpriseSub extends Surprise {
  public void foo() {}
}

public class FlowSensitiveReachabilityTest {
  public static void root() {
    clone(null);
    get_field(null);
    legacy(null);
    string_instantiable();
    RegularInstantiable obj = regular_instantiable();
    obj.foo();
    ((Intermediate)obj).foo();
    ((Base)obj).foo();
  }

  static DataHolder clone(DataHolder data_holder) {
    // constructor can only be reached if we already had a constructed non-null instance
    Data data = data_holder.get_data();
    return new DataHolder(data);
  }

  static Data get_field(DataHolder data_holder) {
    // field is not needed if we don't have a constructed instance
    return data_holder.m_data;
  }

  static LegacyInstantiable legacy(Object o) {
    // instanceof and check-cast used to make the object "dynamically referenced".
    if (o instanceof LegacyInstantiable) {
      return (LegacyInstantiable)o;
    }
    return null;
  }

  static String string_instantiable() {
    return "StringInstantiable";
  }

  static RegularInstantiable regular_instantiable() {
    return new RegularInstantiable();
  }

  public static void abstract_overrides_non_abstract() {
    SurpriseBase base = new SurpriseSub();
    base.foo();
  }
}
