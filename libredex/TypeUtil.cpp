/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeUtil.h"

namespace known_types {

DexType* _void() { return DexType::make_type("V"); }

DexType* _byte() { return DexType::make_type("B"); }

DexType* _char() { return DexType::make_type("C"); }

DexType* _short() { return DexType::make_type("S"); }

DexType* _int() { return DexType::make_type("I"); }

DexType* _long() { return DexType::make_type("J"); }

DexType* _boolean() { return DexType::make_type("Z"); }

DexType* _float() { return DexType::make_type("F"); }

DexType* _double() { return DexType::make_type("D"); }

DexType* java_lang_String() { return DexType::make_type("Ljava/lang/String;"); }

DexType* java_lang_Class() { return DexType::make_type("Ljava/lang/Class;"); }

DexType* java_lang_Enum() { return DexType::make_type("Ljava/lang/Enum;"); }

DexType* java_lang_Object() { return DexType::make_type("Ljava/lang/Object;"); }

DexType* java_lang_Throwable() {
  return DexType::make_type("Ljava/lang/Throwable;");
}

DexType* java_lang_Boolean() {
  return DexType::make_type("Ljava/lang/Boolean;");
}

DexType* java_lang_Byte() { return DexType::make_type("Ljava/lang/Byte;"); }

DexType* java_lang_Short() { return DexType::make_type("Ljava/lang/Short;"); }

DexType* java_lang_Character() {
  return DexType::make_type("Ljava/lang/Character;");
}

DexType* java_lang_Integer() {
  return DexType::make_type("Ljava/lang/Integer;");
}

DexType* java_lang_Long() { return DexType::make_type("Ljava/lang/Long;"); }

DexType* java_lang_Float() { return DexType::make_type("Ljava/lang/Float;"); }

DexType* java_lang_Double() { return DexType::make_type("Ljava/lang/Double;"); }

}; // namespace known_types
