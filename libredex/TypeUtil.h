/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace known_types {

DexType* _void();

DexType* _byte();

DexType* _char();

DexType* _short();

DexType* _int();

DexType* _long();

DexType* _boolean();

DexType* _float();

DexType* _double();

DexType* java_lang_String();

DexType* java_lang_Class();

DexType* java_lang_Enum();

DexType* java_lang_Object();

DexType* java_lang_Throwable();

DexType* java_lang_Boolean();

DexType* java_lang_Byte();

DexType* java_lang_Short();

DexType* java_lang_Character();

DexType* java_lang_Integer();

DexType* java_lang_Long();

DexType* java_lang_Float();

DexType* java_lang_Double();

}; // namespace known_types
