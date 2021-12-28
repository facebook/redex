/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexMemberRefs.h"

namespace java_declarations {

/*
 * @param line Java field declaration
 * @return Field name, field type in external simple form
 *
 * Support field declaration syntax: <modifier>* <field_type> <field_name>;
 * "public final int myField;"
 * "Object myField;"
 *
 * 1. Do not have space in field type, for example, use "Set<Integer>" instead
 * of "Set <Integer>"
 * 2. The brackets identify the array type and should appear with the type
 * designation, for example, use "int[] myField" instead of "int myField[]"
 * TODO: have more robust support for above 1 and 2
 */
dex_member_refs::FieldDescriptorTokens parse_field_declaration(
    std::string_view line);

/*
 * @param line Java method declaration
 * @return method name, argument types, return type in external simple form
 *
 * Support method declaration syntax: <modifier>* <return_type> <method_name>
 * (<parameter_type parameter_name>*) <throw>*;
 * "public synchronized int foo()"
 * "void bar(String x) throw Exception"
 *
 * 1. Do not have space in type, for example, use "Set<Integer>" instead
 * of "Set <Integer>"
 * 2. The brackets identify the array type and should appear with the type
 * designation, for example, use "int[] myField" instead of "int myField[]"
 * TODO: have more robust support for above 1 and 2
 */
dex_member_refs::MethodDescriptorTokens parse_method_declaration(
    std::string_view line);
} // namespace java_declarations
