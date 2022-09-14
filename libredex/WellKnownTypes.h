/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
  FOR_EACH(java_lang_Double, "Ljava/lang/Double;")       \
  FOR_EACH(java_lang_RuntimeException, "Ljava/lang/RuntimeException;")

#define PRIMITIVE_PSEUDO_TYPE_FIELDS                                       \
  FOR_EACH(Void_TYPE, "Ljava/lang/Void;.TYPE:Ljava/lang/Class;")           \
  FOR_EACH(Byte_TYPE, "Ljava/lang/Byte;.TYPE:Ljava/lang/Class;")           \
  FOR_EACH(Character_TYPE, "Ljava/lang/Character;.TYPE:Ljava/lang/Class;") \
  FOR_EACH(Short_TYPE, "Ljava/lang/Short;.TYPE:Ljava/lang/Class;")         \
  FOR_EACH(Integer_TYPE, "Ljava/lang/Integer;.TYPE:Ljava/lang/Class;")     \
  FOR_EACH(Long_TYPE, "Ljava/lang/Long;.TYPE:Ljava/lang/Class;")           \
  FOR_EACH(Boolean_TYPE, "Ljava/lang/Boolean;.TYPE:Ljava/lang/Class;")     \
  FOR_EACH(Float_TYPE, "Ljava/lang/Float;.TYPE:Ljava/lang/Class;")         \
  FOR_EACH(Double_TYPE, "Ljava/lang/Double;.TYPE:Ljava/lang/Class;")

#define WELL_KNOWN_METHODS                                                    \
  FOR_EACH(java_lang_Object_ctor, "Ljava/lang/Object;.<init>:()V")            \
  FOR_EACH(java_lang_Enum_ctor,                                               \
           "Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V")                  \
  FOR_EACH(java_lang_Enum_ordinal, "Ljava/lang/Enum;.ordinal:()I")            \
  FOR_EACH(java_lang_Enum_name, "Ljava/lang/Enum;.name:()Ljava/lang/String;") \
  FOR_EACH(java_lang_Enum_equals,                                             \
           "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z")                   \
  FOR_EACH(java_lang_Integer_valueOf,                                         \
           "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")              \
  FOR_EACH(java_lang_Integer_intValue, "Ljava/lang/Integer;.intValue:()I")    \
  FOR_EACH(java_lang_Throwable_fillInStackTrace,                              \
           "Ljava/lang/Throwable;.fillInStackTrace:()Ljava/lang/Throwable;")  \
  FOR_EACH(java_lang_RuntimeException_init_String,                            \
           "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
