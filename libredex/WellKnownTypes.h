/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// (name, java_name)
#define WELL_KNOWN_TYPES                                 \
  FOR_EACH(_void, "V")                                   \
  FOR_EACH(_byte, "B")                                   \
  FOR_EACH(_char, "C")                                   \
  FOR_EACH(_short, "S")                                  \
  FOR_EACH(_int, "I")                                    \
  FOR_EACH(_long, "J")                                   \
  FOR_EACH(_boolean, "Z")                                \
  FOR_EACH(_float, "F")                                  \
  FOR_EACH(_double, "D")                                 \
  FOR_EACH(java_lang_String, "Ljava/lang/String;")       \
  FOR_EACH(java_lang_Class, "Ljava/lang/Class;")         \
  FOR_EACH(java_lang_Enum, "Ljava/lang/Enum;")           \
  FOR_EACH(java_lang_Object, "Ljava/lang/Object;")       \
  FOR_EACH(java_lang_Void, "Ljava/lang/Void;")           \
  FOR_EACH(java_lang_Throwable, "Ljava/lang/Throwable;") \
  FOR_EACH(java_lang_Boolean, "Ljava/lang/Boolean;")     \
  FOR_EACH(java_lang_Byte, "Ljava/lang/Byte;")           \
  FOR_EACH(java_lang_Short, "Ljava/lang/Short;")         \
  FOR_EACH(java_lang_Character, "Ljava/lang/Character;") \
  FOR_EACH(java_lang_Integer, "Ljava/lang/Integer;")     \
  FOR_EACH(java_lang_Long, "Ljava/lang/Long;")           \
  FOR_EACH(java_lang_Float, "Ljava/lang/Float;")         \
  FOR_EACH(java_lang_Double, "Ljava/lang/Double;")
