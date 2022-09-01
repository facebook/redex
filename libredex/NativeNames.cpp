/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NativeNames.h"

#include <boost/algorithm/string.hpp>
#include <cctype>
#include <sstream>

// Implements name generation for JNI methods according to the spec.
// https://docs.oracle.com/javase/1.5.0/docs/guide/jni/spec/design.html

namespace native_names {

// By standard, Java allows alphanumerical characters, unicode characters, as
// well as _ and $ to be used in the identifiers.
// NOTE: This doesn't yet support Unicode characters, even though "$" is
// represented using Unicode.
void escape_char(std::ostringstream& out, char c) {
  if (isalnum(c)) {
    out << c;
    return;
  }

  switch (c) {
  case '_':
    out << "_1";
    break;
  case '$':
    out << "_00024";
    break;
  default:
    always_assert_log(false, "No mapping for char %c!", c);
  }
}

void escape_single_identifier(std::ostringstream& out, std::string_view name) {
  for (char c : name) {
    escape_char(out, c);
  }
}

void mangle_class_name(std::ostringstream& out, std::string_view cls_name) {
  always_assert(boost::starts_with(cls_name, "L"));
  always_assert(boost::ends_with(cls_name, ";"));
  cls_name.remove_prefix(1);
  cls_name.remove_suffix(1);

  for (char c : cls_name) {
    if (c == '/') {
      out << "_";
    } else {
      escape_char(out, c);
    }
  }
}

void mangle_type_name_in_signature(std::ostringstream& out,
                                   std::string_view type_name) {
  for (char c : type_name) {
    switch (c) {
    case '/':
      out << "_";
      break;
    case ';':
      out << "_2";
      break;
    case '[':
      out << "_3";
      break;
    default:
      escape_char(out, c);
    }
  }
}

void get_native_short_name_for_method_impl(std::ostringstream& out,
                                           DexMethodRef* method) {
  out << "Java_";
  mangle_class_name(out, method->get_class()->str());
  out << "_";
  escape_single_identifier(out, method->get_name()->str());
}

void get_native_long_name_for_method_impl(std::ostringstream& out,
                                          DexMethodRef* method) {
  get_native_short_name_for_method_impl(out, method);
  out << "__";
  DexTypeList* types = method->get_proto()->get_args();
  for (DexType* type : *types) {
    mangle_type_name_in_signature(out, type->get_name()->str());
  }
}

std::string get_native_short_name_for_method(DexMethodRef* method) {
  std::ostringstream out;
  get_native_short_name_for_method_impl(out, method);
  return out.str();
}

std::string get_native_long_name_for_method(DexMethodRef* method) {
  std::ostringstream out;
  get_native_long_name_for_method_impl(out, method);
  return out.str();
}

} // namespace native_names
