/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardConfiguration.h"

#include <algorithm>
#include <boost/functional/hash.hpp>

#include "StlUtil.h"

namespace keep_rules {

namespace {

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
} // namespace

bool operator==(const MemberSpecification& lhs,
                const MemberSpecification& rhs) {
  return lhs.requiredSetAccessFlags == rhs.requiredSetAccessFlags &&
         lhs.requiredUnsetAccessFlags == rhs.requiredUnsetAccessFlags &&
         lhs.annotationType == rhs.annotationType && lhs.name == rhs.name &&
         lhs.descriptor == rhs.descriptor;
}

size_t hash_value(const MemberSpecification& spec) {
  size_t seed{0};
  boost::hash_combine(seed, spec.requiredSetAccessFlags);
  boost::hash_combine(seed, spec.requiredUnsetAccessFlags);
  boost::hash_combine(seed, spec.annotationType);
  boost::hash_combine(seed, spec.name);
  boost::hash_combine(seed, spec.descriptor);
  return seed;
}

bool operator==(const ClassSpecification& lhs, const ClassSpecification& rhs) {
  return lhs.classNames == rhs.classNames &&
         lhs.annotationType == rhs.annotationType &&
         lhs.extendsClassName == rhs.extendsClassName &&
         lhs.extendsAnnotationType == rhs.extendsAnnotationType &&
         lhs.setAccessFlags == rhs.setAccessFlags &&
         lhs.unsetAccessFlags == rhs.unsetAccessFlags &&
         lhs.fieldSpecifications == rhs.fieldSpecifications &&
         lhs.methodSpecifications == rhs.methodSpecifications;
}

size_t hash_value(const ClassSpecification& spec) {
  size_t seed{0};
  for (const auto& v : spec.classNames) {
    boost::hash_combine(seed, v.negated);
    boost::hash_combine(seed, v.name);
  }
  boost::hash_combine(seed, spec.annotationType);
  boost::hash_combine(seed, spec.extendsClassName);
  boost::hash_combine(seed, spec.extendsAnnotationType);
  boost::hash_combine(seed, spec.setAccessFlags);
  boost::hash_combine(seed, spec.unsetAccessFlags);
  boost::hash_combine(seed, spec.fieldSpecifications);
  boost::hash_combine(seed, spec.methodSpecifications);
  return seed;
}

bool operator==(const KeepSpec& lhs, const KeepSpec& rhs) {
  return lhs.includedescriptorclasses == rhs.includedescriptorclasses &&
         lhs.allowshrinking == rhs.allowshrinking &&
         lhs.allowoptimization == rhs.allowoptimization &&
         lhs.allowobfuscation == rhs.allowobfuscation &&
         lhs.class_spec == rhs.class_spec;
}

std::ostream& operator<<(std::ostream& text, const KeepSpec& keep_rule) {
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
  return text;
}

size_t hash_value(const KeepSpec& spec) {
  size_t seed{0};
  boost::hash_combine(seed, spec.includedescriptorclasses);
  boost::hash_combine(seed, spec.allowshrinking);
  boost::hash_combine(seed, spec.allowoptimization);
  boost::hash_combine(seed, spec.allowobfuscation);
  boost::hash_combine(seed, spec.class_spec);
  return seed;
}

void KeepSpecSet::erase_if(const std::function<bool(const KeepSpec&)>& pred) {
  std::unordered_set<const KeepSpec*> erased;
  std20::erase_if(m_unordered_set, [&](auto& v) {
    if (pred(*v)) {
      erased.emplace(v.get());
      return true;
    }
    return false;
  });
  m_ordered.erase(
      std::remove_if(m_ordered.begin(),
                     m_ordered.end(),
                     [&](const KeepSpec* ks) { return erased.count(ks); }),
      m_ordered.end());
}

} // namespace keep_rules
