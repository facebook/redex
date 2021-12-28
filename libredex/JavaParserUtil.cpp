/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

std::string_view next_token(std::string_view& line) {
  auto first_non_space = line.find_first_not_of(" ");
  if (first_non_space == std::string_view::npos) {
    first_non_space = line.size();
  }
  line.remove_prefix(first_non_space);

  if (!line.empty() && is_separator(line.front())) {
    auto ret = line.substr(0, 1);
    line.remove_prefix(1);
    return ret;
  }

  size_t next_token_size{0};
  for (char c : line) {
    if (is_separator(c)) {
      break;
    } else {
      next_token_size++;
    }
  }
  std::string_view ret = line.substr(0, next_token_size);
  line.remove_prefix(next_token_size);
  return ret;
}

const std::unordered_set<std::string_view> JAVA_ACCESS_MODIFIERS = {
    "public", "protected", "private"};
const std::string_view JAVA_STATIC_MODIFIER = "static";
const std::string_view JAVA_FINAL_MODIFIER = "final";
const std::string_view JAVA_TRANSIENT_MODIFIER = "transient";
const std::string_view JAVA_VOLATILE_MODIFIER = "volatile";
const std::string_view JAVA_ABSTRACT_MODIFIER = "abstract";
const std::string_view JAVA_SYNCHRONIZED_MODIFIER = "synchronized";
const std::string_view JAVA_NATIVE_MODIFIER = "native";
const std::string_view JAVA_STRICTFP_MODIFIER = "strictfp";

bool is_field_modifier(std::string_view token) {
  const static std::unordered_set<std::string_view> field_modifiers{
      JAVA_STATIC_MODIFIER,
      JAVA_FINAL_MODIFIER,
      JAVA_TRANSIENT_MODIFIER,
      JAVA_VOLATILE_MODIFIER,
  };
  return JAVA_ACCESS_MODIFIERS.count(token) || field_modifiers.count(token);
}

bool is_method_modifier(std::string_view token) {
  const static std::unordered_set<std::string_view> method_modifiers{
      JAVA_STATIC_MODIFIER,       JAVA_FINAL_MODIFIER,  JAVA_ABSTRACT_MODIFIER,
      JAVA_SYNCHRONIZED_MODIFIER, JAVA_NATIVE_MODIFIER, JAVA_STRICTFP_MODIFIER,
  };
  return JAVA_ACCESS_MODIFIERS.count(token) || method_modifiers.count(token);
}

dex_member_refs::FieldDescriptorTokens parse_field_declaration(
    std::string_view line) {
  bool parsed_type = false;
  dex_member_refs::FieldDescriptorTokens fdt;

  while (!line.empty() && line.front() != ';') {
    std::string_view token = next_token(line);
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

std::vector<std::string_view> parse_arguments(std::string_view line) {
  std::vector<std::string_view> args;
  bool parsed_type = false;
  while (!line.empty() && line.front() != ';') {
    auto token = next_token(line);
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
    std::string_view line) {
  bool parsed_type = false;
  dex_member_refs::MethodDescriptorTokens mdt;

  std::string_view subline = line.substr(0, line.find('('));
  while (!subline.empty() && subline.front() != ';') {
    auto token = next_token(subline);
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
    mdt.rtype = std::string_view();
  }
  always_assert_log(!mdt.name.empty(), "Could not find function name");

  std::string_view args_str =
      line.substr(line.find('(') + 1, line.find(')') - line.find('(') - 1);
  mdt.args = parse_arguments(args_str);

  return mdt;
}
} // namespace java_declarations
