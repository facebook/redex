/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JavaParserUtil.h"
#include "Debug.h"

#include <unordered_set>

namespace java_declarations {

static bool is_separator(const char c) {
  return c == '\0' || c == ' ' || c == ':' || c == ',' || c == '\n' ||
         c == '(' || c == ')' || c == ';';
}

std::string next_token(const char** p) {
  while (**p == ' ') {
    (*p)++;
  }

  if (is_separator(**p)) {
    return std::string(1, *(*p)++);
  }

  const char* begin = *p;
  while (**p && !is_separator(**p)) {
    (*p)++;
  }
  // TODO: Make this more efficient
  return std::string(begin, *p);
}

const std::unordered_set<std::string> JAVA_ACCESS_MODIFIERS = {
    "public", "protected", "private"};
const std::string JAVA_STATIC_MODIFIER = "static";
const std::string JAVA_FINAL_MODIFIER = "final";
const std::string JAVA_TRANSIENT_MODIFIER = "transient";
const std::string JAVA_VOLATILE_MODIFIER = "volatile";
const std::string JAVA_ABSTRACT_MODIFIER = "abstract";
const std::string JAVA_SYNCHRONIZED_MODIFIER = "synchronized";
const std::string JAVA_NATIVE_MODIFIER = "native";
const std::string JAVA_STRICTFP_MODIFIER = "strictfp";

bool is_field_modifier(const std::string& token) {
  return JAVA_ACCESS_MODIFIERS.count(token) || token == JAVA_STATIC_MODIFIER ||
         token == JAVA_FINAL_MODIFIER || token == JAVA_TRANSIENT_MODIFIER ||
         token == JAVA_VOLATILE_MODIFIER;
}

bool is_method_modifier(const std::string& token) {
  return JAVA_ACCESS_MODIFIERS.count(token) || token == JAVA_STATIC_MODIFIER ||
         token == JAVA_FINAL_MODIFIER || token == JAVA_ABSTRACT_MODIFIER ||
         token == JAVA_SYNCHRONIZED_MODIFIER || token == JAVA_NATIVE_MODIFIER ||
         token == JAVA_STRICTFP_MODIFIER;
}

dex_member_refs::FieldDescriptorTokens parse_field_declaration(
    const std::string& line) {
  bool parsed_type = false;
  dex_member_refs::FieldDescriptorTokens fdt;

  auto p = line.c_str();
  while (*p != '\0' && *p != ';') {
    std::string token = next_token(&p);
    if (is_field_modifier(token)) {
      continue;
    }
    if (parsed_type) {
      fdt.name = token;
    } else {
      fdt.type = token;
      parsed_type = true;
    }
  }
  always_assert_log(!fdt.name.empty(), "Could not find field name");
  always_assert_log(!fdt.type.empty(), "Could not find field type");
  return fdt;
}

std::vector<std::string> parse_arguments(const std::string& line) {
  std::vector<std::string> args;
  bool parsed_type = false;
  auto p = line.c_str();
  while (*p != '\0' && *p != ';') {
    std::string token = next_token(&p);
    if (token == ",") {
      parsed_type = false;
    } else {
      if (!parsed_type) {
        args.push_back(token);
        parsed_type = true;
      }
    }
  }
  return args;
}

dex_member_refs::MethodDescriptorTokens parse_method_declaration(
    const std::string& line) {
  bool parsed_type = false;
  dex_member_refs::MethodDescriptorTokens mdt;

  std::string subline = line.substr(0, line.find('('));
  auto p = subline.c_str();
  while (*p != '\0' && *p != ';') {
    std::string token = next_token(&p);
    if (is_method_modifier(token)) {
      continue;
    }
    if (parsed_type) {
      mdt.name = token;
    } else {
      mdt.rtype = token;
      parsed_type = true;
    }
  }

  // it is constructor
  if (mdt.name.empty()) {
    mdt.name = mdt.rtype;
    mdt.rtype = std::string();
  }
  always_assert_log(!mdt.name.empty(), "Could not find function name");

  std::string args_str =
      line.substr(line.find('(') + 1, line.find(')') - line.find('(') - 1);
  mdt.args = parse_arguments(args_str);

  return mdt;
}
} // namespace java_declarations
