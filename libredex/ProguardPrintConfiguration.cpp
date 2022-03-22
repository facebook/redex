/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"

#include <ostream>
#include <sstream>

std::string show_keep_style(const keep_rules::KeepSpec& keep_rule) {
  if (keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      !keep_rule.allowshrinking) {
    return "-keep";
  }
  if (!keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      !keep_rule.allowshrinking) {
    return "-keepclassmembers";
  }
  if (!keep_rule.mark_classes && keep_rule.mark_conditionally &&
      !keep_rule.allowshrinking) {
    return "-keepclasseswithmembers";
  }
  if (keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      keep_rule.allowshrinking) {
    return "-keepnames";
  }
  if (!keep_rule.mark_classes && !keep_rule.mark_conditionally &&
      keep_rule.allowshrinking) {
    return "-keepclassmembernames";
  }
  if (!keep_rule.mark_classes && keep_rule.mark_conditionally &&
      keep_rule.allowshrinking) {
    return "-keepclasseswithmembernames";
  }
  return "-invalidkeep";
}

std::string show_keep_modifiers(const keep_rules::KeepSpec& keep_rule) {
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

std::string show_access(const DexAccessFlags access, bool isMethod) {
  switch (access) {
  case ACC_PUBLIC:
    return "public";
  case ACC_PRIVATE:
    return "private";
  case ACC_PROTECTED:
    return "protected";
  case ACC_STATIC:
    return "static";
  case ACC_FINAL:
    return "final";
  case ACC_INTERFACE:
    return "interface";
  case ACC_SYNCHRONIZED:
    return "synchronized";
  case ACC_VOLATILE: // or ACC_BRIDGE
    return isMethod ? "bridge" : "volatile";
  case ACC_TRANSIENT: // or ACC_VARARGS
    return isMethod ? "varargs" : "transient";
  case ACC_NATIVE:
    return "native";
  case ACC_ABSTRACT:
    return "abstract";
  case ACC_STRICT:
    return "strict";
  case ACC_SYNTHETIC:
    return "synthetic";
  case ACC_ANNOTATION:
    return "@interface";
  case ACC_ENUM:
    return "enum";
  default:
    return "";
  }
}

std::string show_access_flags(const DexAccessFlags flags,
                              const DexAccessFlags negated_flags,
                              bool isMethod) {
  std::ostringstream ss;
  for (int offset = 0; offset < 32; offset++) {
    const DexAccessFlags access = static_cast<DexAccessFlags>(1 << offset);
    if ((flags & access) == 0) {
      continue;
    }
    if (is_interface(access)) {
      ss << "@";
    }
    ss << show_access(access, isMethod) << " ";
  }
  for (int offset = 0; offset < 32; offset++) {
    const DexAccessFlags access = static_cast<DexAccessFlags>(1 << offset);
    if ((negated_flags & access) == 0) {
      continue;
    }
    ss << "!";
    if (is_interface(access)) {
      ss << "@";
    }
    ss << show_access(access, isMethod) << " ";
  }
  return ss.str();
}

std::string show_fields(
    const std::vector<keep_rules::MemberSpecification>& fields) {
  std::ostringstream ss;
  for (const auto& field : fields) {
    if (!(field.annotationType.empty())) {
      ss << "@" << field.annotationType << " ";
    }
    ss << show_access_flags(
        field.requiredSetAccessFlags, field.requiredUnsetAccessFlags, false);
    auto name = field.name.empty() ? "*" : field.name;
    ss << field.descriptor << " " << name << "; ";
  }
  return ss.str();
}

std::string show_methods(
    const std::vector<keep_rules::MemberSpecification>& methods) {
  std::ostringstream ss;
  for (const auto& method : methods) {
    if (!(method.annotationType.empty())) {
      ss << "@" << method.annotationType << " ";
    }
    ss << show_access_flags(
        method.requiredSetAccessFlags, method.requiredUnsetAccessFlags, true);
    auto name = method.name.empty() ? "*" : method.name;
    ss << method.descriptor << " " << name << "(); ";
  }
  return ss.str();
}

std::string keep_rules::show_keep(const KeepSpec& keep_rule, bool show_source) {
  std::ostringstream text;
  text << show_keep_style(keep_rule) << show_keep_modifiers(keep_rule) << " ";
  const auto class_spec = keep_rule.class_spec;
  if (!(class_spec.annotationType.empty())) {
    text << "@" << class_spec.annotationType << " ";
  }
  text << show_access_flags(
      class_spec.setAccessFlags, class_spec.unsetAccessFlags, false);
  if (is_annotation(class_spec.setAccessFlags)) {
    if (is_enum(class_spec.setAccessFlags)) {
      if (is_interface(class_spec.setAccessFlags)) {
        text << "interface ";
      } else {
        text << "class ";
      }
    }
  }
  for (std::size_t i = 0; i < class_spec.classNames.size(); i++) {
    text << (class_spec.classNames[i].negated ? "!" : "")
         << class_spec.classNames[i].name;
    text << (i == class_spec.classNames.size() - 1 ? " " : ", ");
  }
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

  if (show_source) {
    std::ostringstream source;
    source << keep_rule.source_filename << ":" << keep_rule.source_line;
    return '\'' + text.str() + "\' from " + source.str();
  }
  return text.str();
}

void keep_rules::show_configuration(
    std::ostream& output,
    const Scope& classes,
    const keep_rules::ProguardConfiguration& config) {
  size_t total = classes.size();
  for (const auto& cls : classes) {
    total += cls->get_vmethods().size() + cls->get_dmethods().size() +
             cls->get_ifields().size() + cls->get_sfields().size();
  }
  for (const auto& keep : config.keep_rules) {
    output << keep_rules::show_keep(*keep) << std::endl;
  }
}
