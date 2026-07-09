# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

-dontobfuscate
-dontshrink

# Keep the @Test entry points as roots.
-keepclassmembers class * {
  @org.junit.Test *;
}

# Standard Serializable keep rule. Its `!static !transient <fields>` clause
# excludes transient fields, so `buffer` is not kept and remains optimizable.
-keepclassmembers class ** implements java.io.Serializable {
  static final long serialVersionUID;
  private static final java.io.ObjectStreamField[] serialPersistentFields;
  !static !transient <fields>;
  private void writeObject(java.io.ObjectOutputStream);
  private void readObject(java.io.ObjectInputStream);
  java.lang.Object writeReplace();
  java.lang.Object readResolve();
}
