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

void print_fields(std::ostream& output,
                  const ProguardMap& pg_map,
                  const std::string& class_name,
                  const std::list<DexField*>& fields) {
  for (const auto& field : fields) {
    if (keep(field) || keepclassmembers(field)) {
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
  }
}

std::string extract_suffix(std::string class_name) {
  auto i = class_name.find_last_of(".");
  if (i == std::string::npos) {
    // This is a class name with no package prefix.
    return class_name;
  }
  return class_name.substr(i + 1);
}

void print_methods(std::ostream& output,
                   const ProguardMap& pg_map,
                   const std::string& class_name,
                   const std::list<DexMethod*>& methods) {
  for (const auto& method : methods) {
    if (keep(method) || keepclassmembers(method)) {
      std::string method_name =
          extract_method_name(method->get_name()->c_str());
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
  }
}

// Print out the seeds computed in classes by Redex to the specified ostream.
// The ProGuard map is used to help deobfuscate type descriptors.
void redex::print_seeds(std::ostream& output,
                        const ProguardMap& pg_map,
                        const Scope& classes) {
  for (const auto& cls : classes) {
    if (keep(cls) || keepclassmembers(cls)) {
      auto deob = cls->get_deobfuscated_name();
      if (deob.empty()) {
        std::cerr << "WARNING: this class has no deobu name: "
                  << cls->get_name()->c_str() << std::endl;
        deob = cls->get_name()->c_str();
      }
      std::string name = dexdump_name_to_dot_name(deob);
      if (keep(cls)) {
        output << name << std::endl;
      }
      print_fields(output, pg_map, name, cls->get_ifields());
      print_fields(output, pg_map, name, cls->get_sfields());
      print_methods(output, pg_map, name, cls->get_dmethods());
      print_methods(output, pg_map, name, cls->get_vmethods());
    }
  }
}
