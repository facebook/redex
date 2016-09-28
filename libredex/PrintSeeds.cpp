/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PrintSeeds.h"
#include "ReachableClasses.h"
#include "ReferencedState.h"

std::string dexdump_name_to_dot_name(const std::string& dexdump_name) {
  assert(!dexdump_name.empty());
  std::string s;
  for (const char& ch : dexdump_name.substr(1)) {
    if (ch == '/') {
      s += '.';
      continue;
    }
    if (ch == ';') {
      continue;
    }
    s += ch;
  }
  return s;
}

std::string type_descriptor_to_java(const std::string& descriptor) {
  assert(!descriptor.empty());
  if (descriptor[0] == '[') {
    return type_descriptor_to_java(descriptor.substr(1)) + "[]";
  }
  if (descriptor == "B") {
    return "byte";
  }
  if (descriptor == "S") {
    return "short";
  }
  if (descriptor == "I") {
    return "int";
  }
  if (descriptor == "J") {
    return "long";
  }
  if (descriptor == "C") {
    return "char";
  }
  if (descriptor == "F") {
    return "float";
  }
  if (descriptor == "D") {
    return "double";
  }
  if (descriptor == "Z") {
    return "boolean";
  }
  if (descriptor == "V") {
    return "void";
  }
  if (descriptor[0] == 'L') {
    return dexdump_name_to_dot_name(descriptor);
  }
  std::cerr
      << "type_descriptor_to_java: unexpected type descriptor " + descriptor
      << std::endl;
  exit(2);
}

std::string extract_field_name(std::string qualified) {
  auto semicolon = qualified.find(";");
  auto colon = qualified.find(":");
  return qualified.substr(semicolon + 2, colon - semicolon - 2);
}

std::string extract_method_name(std::string qualified) {
  auto dot = qualified.find(".");
  auto open = qualified.find("(");
  return qualified.substr(dot + 1, open - dot - 1);
}

std::string form_java_args(const std::list<DexType*>& args) {
  std::string s;
  unsigned long i = 0;
  for (const auto& arg : args) {
    s += type_descriptor_to_java(arg->get_name()->c_str());
    if (i < args.size() - 1) {
      s += ",";
    }
    i++;
  }
  return s;
}

std::string java_args(std::list<DexType*>& args) {
  return "(" + form_java_args(args) + ")";
}

void print_fields(std::ostream& output,
                  const std::string& class_name,
                  const std::list<DexField*>& fields) {
  for (const auto& field : fields) {
    if (keep(field)) {
      output << class_name << ": "
             << type_descriptor_to_java(field->get_type()->get_name()->c_str())
             << " " << extract_field_name(field->get_deobfuscated_name())
             << std::endl;
    }
  }
}

std::string extract_suffix(std::string class_name) {
  auto i = class_name.find_last_of(".");
  assert(i != std::string::npos);
  return class_name.substr(i + 1);
}

void print_methods(std::ostream& output,
                   const std::string& class_name,
                   const std::list<DexMethod*>& methods) {
  for (const auto& method : methods) {
    if (keep(method)) {
      std::string method_name =
          extract_method_name(method->get_name()->c_str());
      if (method_name == "<clinit>") {
        continue;
      }
      bool is_constructor{false};
      if (method_name == "<init>") {
        method_name = extract_suffix(class_name);
        is_constructor = true;
      }
      auto proto = method->get_proto();
      auto args = proto->get_args()->get_type_list();
      auto return_type = proto->get_rtype();
      output << class_name << ": ";
      if (!is_constructor) {
        output << type_descriptor_to_java(return_type->get_name()->c_str())
               << " ";
      }
      output << method_name << java_args(args) << std::endl;
    }
  }
}

void redex::print_seeds(std::ostream& output, const Scope& classes) {
  for (const auto& cls : classes) {
    if (keep(cls)) {
      auto name = dexdump_name_to_dot_name(cls->get_deobfuscated_name());
      output << name << std::endl;
      print_fields(output, name, cls->get_ifields());
      print_fields(output, name, cls->get_sfields());
      print_methods(output, name, cls->get_dmethods());
      print_methods(output, name, cls->get_vmethods());
    }
  }
}
