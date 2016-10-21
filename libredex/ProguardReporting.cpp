/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ProguardReporting.h"
#include "DexClass.h"
#include "ReachableClasses.h"

std::string extract_suffix(std::string class_name) {
  auto i = class_name.find_last_of(".");
  if (i == std::string::npos) {
    // This is a class name with no package prefix.
    return class_name;
  }
  return class_name.substr(i + 1);
}

std::string redex::dexdump_name_to_dot_name(const std::string& dexdump_name) {
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
    return redex::dexdump_name_to_dot_name(descriptor);
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
  auto open = qualified.find(":");
  return qualified.substr(dot + 1, open - dot - 1);
}

// Convert a type descriptor that may contain obfuscated class names
// into the corresponding type descriptor with the class types deobfuscated.
// The incomming type descriptor is a chain of types which may be primitive
// types, array types or class types. For example [[A; -> [[Lcom.wombat.Numbat;
std::string deobfuscate_type_descriptor(const ProguardMap& pg_map,
                                        const std::string& desc) {
  assert(!desc.empty());
  std::string deob;
  size_t i = 0;
  while (i < desc.size()) {
    if (desc[i] == 'L') {
      auto colon = desc.find(";");
      assert(colon != std::string::npos);
      auto class_type = desc.substr(i, colon + 1);
      auto deob_class = pg_map.deobfuscate_class(class_type);
      if (deob_class.empty()) {
        std::cerr << "Warning: failed to deobfuscate class " << class_type
                  << std::endl;
        deob_class = class_type;
      }
      deob += deob_class;
      i = colon + 1;
      continue;
    }
    deob += desc[i];
    i++;
  }
  return deob;
}

std::string form_java_args(const ProguardMap& pg_map,
                           const std::list<DexType*>& args) {
  std::string s;
  unsigned long i = 0;
  for (const auto& arg : args) {
    auto desc = arg->get_name()->c_str();
    auto deobfu_desc = deobfuscate_type_descriptor(pg_map, desc);
    s += type_descriptor_to_java(deobfu_desc);
    if (i < args.size() - 1) {
      s += ",";
    }
    i++;
  }
  return s;
}

std::string java_args(const ProguardMap& pg_map, std::list<DexType*>& args) {
  return "(" + form_java_args(pg_map, args) + ")";
}

void redex::print_method(std::ostream& output,
                         const ProguardMap& pg_map,
                         const std::string& class_name,
                         const DexMethod* method) {
  std::string method_name = extract_method_name(method->get_name()->c_str());
  // Record if this is a constriuctor to supress return value printing
  // beforer the method name.
  bool is_constructor{false};
  if (method_name == "<init>") {
    method_name = extract_suffix(class_name);
    is_constructor = true;
  } else {
    auto deob = method->get_deobfuscated_name();
    if (deob.empty()) {
      std::cerr << "WARNING: method has no deobfu: " << method_name
                << std::endl;
      method_name = extract_method_name(method->get_name()->c_str());
    } else {
      method_name = extract_method_name(deob);
    }
  }
  auto proto = method->get_proto();
  auto args = proto->get_args()->get_type_list();
  auto return_type = proto->get_rtype();
  output << class_name << ": ";
  if (!is_constructor) {
    auto return_type_desc = return_type->get_name()->c_str();
    auto deobfu_return_type =
        deobfuscate_type_descriptor(pg_map, return_type_desc);
    output << type_descriptor_to_java(deobfu_return_type) << " ";
  }
  output << method_name << java_args(pg_map, args) << std::endl;
}

void redex::print_methods(std::ostream& output,
                          const ProguardMap& pg_map,
                          const std::string& class_name,
                          const std::list<DexMethod*>& methods) {
  for (const auto& method : methods) {
    redex::print_method(output, pg_map, class_name, method);
  }
}

void redex::print_field(std::ostream& output,
                        const ProguardMap& pg_map,
                        const std::string& class_name,
                        const DexField* field) {
  auto field_name = field->get_deobfuscated_name();
  auto field_type = field->get_type()->get_name()->c_str();
  std::string deobfu_field_type = field_type;
  if (field_type[0] == 'L') {
    deobfu_field_type = pg_map.deobfuscate_class(field_type);
  }
  output << class_name << ": " << type_descriptor_to_java(deobfu_field_type)
         << " " << extract_field_name(field->get_deobfuscated_name())
         << std::endl;
}

void redex::print_fields(std::ostream& output,
                         const ProguardMap& pg_map,
                         const std::string& class_name,
                         const std::list<DexField*>& fields) {
  for (const auto& field : fields) {
    redex::print_field(output, pg_map, class_name, field);
  }
}

void redex::print_class(std::ostream& output,
                        const ProguardMap& pg_map,
                        const DexClass* cls) {
  auto deob = cls->get_deobfuscated_name();
  if (deob.empty()) {
    std::cerr << "WARNING: this class has no deobu name: "
              << cls->get_name()->c_str() << std::endl;
    deob = cls->get_name()->c_str();
  }
  std::string name = redex::dexdump_name_to_dot_name(deob);
  output << name << std::endl;
  print_fields(output, pg_map, name, cls->get_ifields());
  print_fields(output, pg_map, name, cls->get_sfields());
  print_methods(output, pg_map, name, cls->get_dmethods());
  print_methods(output, pg_map, name, cls->get_vmethods());
}

void redex::print_classes(std::ostream& output,
                          const ProguardMap& pg_map,
                          const Scope& classes) {
  for (const auto& cls : classes) {
    if (!cls->is_external()) {
      redex::print_class(output, pg_map, cls);
    }
  }
}

void alert_seeds_in_fields(std::ostream& output,
                           const std::list<DexField*>& fields) {

  for (const auto& field : fields) {
    if (is_seed(field) && !keep(field)) {
      output << "SEEDS ERROR: " << field->get_deobfuscated_name() << std::endl;
    }
    if (!is_seed(field) && keep(field)) {
      output << "FALSE SEED: " << field->get_deobfuscated_name() << std::endl;
    }
  }
}

void alert_seeds_in_methods(std::ostream& output,
                            const std::list<DexMethod*>& methods) {

  for (const auto& method : methods) {
    if (is_seed(method) && !keep(method)) {
      output << "SEEDS ERROR: " << method->get_deobfuscated_name() << std::endl;
    }
    if (!is_seed(method) && keep(method)) {
      output << "FALSE SEED: " << method->get_deobfuscated_name() << std::endl;
    }
  }
}

void alert_seeds(std::ostream& output, const DexClass* cls) {
  if (is_seed(cls) && !keep(cls)) {
    output << "SEEDS ERROR: " << cls->get_deobfuscated_name() << std::endl;
  }
  if (!is_seed(cls) && keep(cls)) {
    output << "FALSE SEED: " << cls->get_deobfuscated_name() << std::endl;
  }
  if ((cls->c_str() != cls->get_deobfuscated_name())  && !allowshrinking(cls)) {
    output << "RENAMED DESPITE KEEP: " << cls->get_deobfuscated_name() << std::endl;
  if (is_seed(cls)) {
    output << "SEED: " << cls->get_deobfuscated_name() << std::endl;
  }
  }
  alert_seeds_in_fields(output, cls->get_ifields());
  alert_seeds_in_fields(output, cls->get_sfields());
  alert_seeds_in_methods(output, cls->get_dmethods());
  alert_seeds_in_methods(output, cls->get_vmethods());
}

void redex::alert_seeds(std::ostream& output, const Scope& classes) {
  for (const auto& cls : classes) {
    if (!cls->is_external()) {
      alert_seeds(output, cls);
    }
  }
}
