/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"

#include <iostream>
#include <sstream>

std::string show_keep_style(const std::string& keep_style,
                            const redex::KeepSpec& keep_rule) {
  const char* ks = keep_style.c_str();
  if (strcmp(ks, "keep") == 0) {
    if (keep_rule.allowshrinking) {
      return "-keepnames";
    }
    return "-keep";
  }

  if (strcmp(ks, "keepclassmember") == 0) {
    if (keep_rule.allowshrinking) {
      return "-keepclassmembernames";
    }
    return "-keepclassmember";
  }

  if (strcmp(ks, "keepclasseswithmembers") == 0) {
    if (keep_rule.allowshrinking) {
      return "-keepclasseswithmembernames";
    }
    return "-keepclasseswithmembers";
  }

  return "-" + keep_style;
}

std::string show_keep_modifiers(const redex::KeepSpec& keep_rule) {
  std::string modifiers;
  if (keep_rule.allowoptimization) {
    modifiers += ",allowoptimization";
  }
  if (keep_rule.allowobfuscation) {
    modifiers += ",allowobfuscation";
  }
  if (keep_rule.includedescriptorclasses) {
    modifiers += ",includedescriptorclasses";
  }
  return modifiers;
}

std::string show_access(const redex::AccessFlag& access) {
  switch (access) {
  case redex::AccessFlag::PUBLIC:
    return "public";
  case redex::AccessFlag::PRIVATE:
    return "private";
  case redex::AccessFlag::PROTECTED:
    return "protected";
  case redex::AccessFlag::STATIC:
    return "static";
  case redex::AccessFlag::FINAL:
    return "final";
  case redex::AccessFlag::INTERFACE:
    return "interface";
  case redex::AccessFlag::SYNCHRONIZED:
    return "synchronized";
  case redex::AccessFlag::VOLATILE:
    return "volatile";
  case redex::AccessFlag::TRANSIENT:
    return "transient";
  case redex::AccessFlag::BRIDGE:
    return "bridge";
  case redex::AccessFlag::VARARGS:
    return "varargs";
  case redex::AccessFlag::NATIVE:
    return "native";
  case redex::AccessFlag::ABSTRACT:
    return "abstract";
  case redex::AccessFlag::STRICT:
    return "strict";
  case redex::AccessFlag::SYNTHETIC:
    return "synthetic";
  case redex::AccessFlag::ANNOTATION:
    return "@interface";
  case redex::AccessFlag::ENUM:
    return "enum";
  case redex::AccessFlag::CONSTRUCTOR:
    return "";
  }
}

std::string show_access_flags(
    const std::set<redex::AccessFlag>& flags,
    const std::set<redex::AccessFlag>& negated_flags) {
  std::stringstream ss;
  for (const auto& access : flags) {
    if (access == redex::AccessFlag::INTERFACE) {
      ss << "@";
    }
    ss << show_access(access) << " ";
  }
  for (const auto& access : negated_flags) {
    ss << "!";
    if (access == redex::AccessFlag::INTERFACE) {
      ss << "@";
    }
    ss << show_access(access) << " ";
  }
  return ss.str();
}

std::string show_fields(const std::vector<redex::MemberSpecification>& fields) {
  std::stringstream ss;
  for (const auto& field : fields) {
    if (!(field.annotationType.empty())) {
      ss << "@" << field.annotationType << " ";
    }
    ss << show_access_flags(field.requiredSetAccessFlags,
                              field.requiredUnsetAccessFlags);
    auto name = field.name.empty() ? "*" : field.name;
    ss << field.descriptor << " " << name << "; ";
  }
  return ss.str();
}

std::string show_methods(
    const std::vector<redex::MemberSpecification>& methods) {
  std::stringstream ss;
  for (const auto& method : methods) {
    if (!(method.annotationType.empty())) {
      ss << "@" << method.annotationType << " ";
    }
    ss << show_access_flags(method.requiredSetAccessFlags,
                              method.requiredUnsetAccessFlags);
    auto name = method.name.empty() ? "*" : method.name;
    ss << method.descriptor << " " << name << "(); ";
  }
  return ss.str();
}

std::string redex::show_keep(const std::string& keep_style,
                             const KeepSpec& keep_rule) {
  std::stringstream text;
  auto field_count = 0;
  for (const auto& field_spec : keep_rule.class_spec.fieldSpecifications) {
    field_count += field_spec.count;
  }
  auto method_count = 0;
  for (const auto& method_spec : keep_rule.class_spec.methodSpecifications) {
    method_count += method_spec.count;
  }
  auto total = keep_rule.count + field_count + method_count;
  text << total << "\t" << keep_rule.count << "\t" << field_count << "\t"
       << method_count << "\t";
  text << show_keep_style(keep_style, keep_rule)
       << show_keep_modifiers(keep_rule) << " ";
  const auto class_spec = keep_rule.class_spec;
  if (!(class_spec.annotationType.empty())) {
    text << "@" << class_spec.annotationType << " ";
  }
  text << show_access_flags(class_spec.setAccessFlags,
                            class_spec.unsetAccessFlags);
  if (class_spec.setAccessFlags.find(redex::AccessFlag::ANNOTATION) ==
      class_spec.setAccessFlags.end()) {
    if (class_spec.setAccessFlags.find(redex::AccessFlag::ENUM) ==
        class_spec.setAccessFlags.end()) {
      if (class_spec.setAccessFlags.find(redex::AccessFlag::INTERFACE) !=
          class_spec.setAccessFlags.end()) {
        text << "interface ";
      } else {
        text << "class ";
      }
    }
  }
  text << class_spec.className << " ";
  if (!class_spec.extendsClassName.empty()) {
    text << "extends ";
    if (!(class_spec.extendsAnnotationType.empty())) {
      text << "@" << class_spec.extendsAnnotationType << " ";
    }
    text << class_spec.extendsClassName << " ";
  }
  if (!class_spec.fieldSpecifications.empty() ||
      !class_spec.methodSpecifications.empty()) {
    text << "{ ";
    text << show_fields(class_spec.fieldSpecifications);
    text << show_methods(class_spec.methodSpecifications);
    text << "}";
  }
  return text.str();
}

void redex::show_configuration(std::ostream& output,
                               const Scope& classes,
                               const redex::ProguardConfiguration& config) {
  size_t total = classes.size();
  for (const auto& cls : classes) {
    total += cls->get_vmethods().size() + cls->get_dmethods().size() +
             cls->get_ifields().size() + cls->get_sfields().size();
  }
  output << "-1\t"
         << "classes: " << classes.size() << " total: " << total << std::endl;
  for (const auto& keep : config.keep_rules) {
    output << redex::show_keep("keep", keep) << std::endl;
  }
  for (const auto& keep : config.keepclasseswithmembers_rules) {
    output << redex::show_keep("keepclasseswithmembers", keep) << std::endl;
  }
  for (const auto& keep : config.keepclassmembers_rules) {
    output << redex::show_keep("keepclassmembers", keep) << std::endl;
  }
}
