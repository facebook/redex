/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <iostream>
#include <boost/regex.hpp>
#include <string>

#include "DexAccess.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardRegex.h"
#include "keeprules.h"

namespace redex {

template <class DexMember>
void apply_keep_modifiers(const KeepSpec& k, DexMember* member) {
  if (k.includedescriptorclasses) {
    member->rstate.set_includedescriptorclasses();
  }
  if (k.allowoptimization) {
    member->rstate.set_allowoptimization();
  }
  if (k.allowshrinking) {
    member->rstate.set_allowshrinking();
  }
  if (k.allowobfuscation &&
      std::string(member->get_name()->c_str()) != "<init>") {
    member->rstate.set_allowobfuscation();
  }
}

bool check_required_access_flags(const std::set<AccessFlag>& requiredSet,
                                 const DexAccessFlags& access_flags) {
  auto require_public = requiredSet.count(AccessFlag::PUBLIC);
  auto require_private = requiredSet.count(AccessFlag::PRIVATE);
  auto require_protectd = requiredSet.count(AccessFlag::PROTECTED);
  bool match_one = require_public + require_private + require_protectd > 0;
  for (const AccessFlag& af : requiredSet) {
    switch (af) {
    case AccessFlag::PUBLIC:
      if (!(access_flags & ACC_PUBLIC)) {
        if (match_one) {
          if (require_private && (access_flags & ACC_PRIVATE)) {
             continue;
          }
          if (require_protectd && (access_flags & ACC_PROTECTED)) {
             continue;
          }
        }
        return false;
      }
      break;
    case AccessFlag::PRIVATE:
      if (!(access_flags & ACC_PRIVATE)) {
        if (match_one) {
          if (require_public && (access_flags & ACC_PUBLIC)) {
             continue;
          }
          if (require_protectd && (access_flags & ACC_PROTECTED)) {
             continue;
          }
        }
        return false;
      }
      break;
    case AccessFlag::PROTECTED:
      if (!(access_flags & ACC_PROTECTED)) {
        if (match_one) {
          if (require_public && (access_flags & ACC_PUBLIC)) {
             continue;
          }
          if (require_private && (access_flags & ACC_PRIVATE)) {
             continue;
          }
        }
        return false;
      }
      break;
    case AccessFlag::STATIC:
      if (!(access_flags & ACC_STATIC)) {
        return false;
      }
      break;
    case AccessFlag::FINAL:
      if (!(access_flags & ACC_FINAL)) {
        return false;
      }
      break;
    case AccessFlag::INTERFACE:
      if (!(access_flags & ACC_INTERFACE)) {
        return false;
      }
      break;
    case AccessFlag::SYNCHRONIZED:
      if (!(access_flags & ACC_SYNCHRONIZED)) {
        return false;
      }
      break;
    case AccessFlag::VOLATILE:
      if (!(access_flags & ACC_VOLATILE)) {
        return false;
      }
      break;
    case AccessFlag::TRANSIENT:
      if (!(access_flags & ACC_TRANSIENT)) {
        return false;
      }
      break;
    case AccessFlag::BRIDGE:
      if (!(access_flags & ACC_BRIDGE)) {
        return false;
      }
      break;
    case AccessFlag::VARARGS:
      if (!(access_flags & ACC_VARARGS)) {
        return false;
      }
      break;
    case AccessFlag::NATIVE:
      if (!(access_flags & ACC_NATIVE)) {
        return false;
      }
      break;
    case AccessFlag::ABSTRACT:
      if (!(access_flags & ACC_ABSTRACT)) {
        return false;
      }
      break;
    case AccessFlag::STRICT:
      if (!(access_flags & ACC_STRICT)) {
        return false;
      }
      break;
    case AccessFlag::SYNTHETIC:
      if (!(access_flags & ACC_SYNTHETIC)) {
        return false;
      }
      break;
    case AccessFlag::ANNOTATION:
      if (!(access_flags & ACC_ANNOTATION)) {
        return false;
      }
      break;
    case AccessFlag::ENUM:
      if (!(access_flags & ACC_ENUM)) {
        return false;
      }
      break;
    }
  }
  return true;
}

bool check_required_unset_access_flags(
    const std::set<AccessFlag>& requiredUnset,
    const DexAccessFlags& access_flags) {
  for (const AccessFlag& af : requiredUnset) {
    switch (af) {
    case AccessFlag::PUBLIC:
      if ((access_flags & ACC_PUBLIC)) {
        return false;
      }
      break;
    case AccessFlag::PRIVATE:
      if ((access_flags & ACC_PRIVATE)) {
        return false;
      }
      break;
    case AccessFlag::PROTECTED:
      if ((access_flags & ACC_PROTECTED)) {
        return false;
      }
      break;
    case AccessFlag::STATIC:
      if ((access_flags & ACC_STATIC)) {
        return false;
      }
      break;
    case AccessFlag::FINAL:
      if ((access_flags & ACC_FINAL)) {
        return false;
      }
      break;
    case AccessFlag::INTERFACE:
      if ((access_flags & ACC_INTERFACE)) {
        return false;
      }
      break;
    case AccessFlag::SYNCHRONIZED:
      if ((access_flags & ACC_SYNCHRONIZED)) {
        return false;
      }
      break;
    case AccessFlag::VOLATILE:
      if ((access_flags & ACC_VOLATILE)) {
        return false;
      }
      break;
    case AccessFlag::TRANSIENT:
      if ((access_flags & ACC_TRANSIENT)) {
        return false;
      }
      break;
    case AccessFlag::BRIDGE:
      if ((access_flags & ACC_BRIDGE)) {
        return false;
      }
      break;
    case AccessFlag::VARARGS:
      if ((access_flags & ACC_VARARGS)) {
        return false;
      }
      break;
    case AccessFlag::NATIVE:
      if ((access_flags & ACC_NATIVE)) {
        return false;
      }
      break;
    case AccessFlag::ABSTRACT:
      if ((access_flags & ACC_ABSTRACT)) {
        return false;
      }
      break;
    case AccessFlag::STRICT:
      if ((access_flags & ACC_STRICT)) {
        return false;
      }
      break;
    case AccessFlag::SYNTHETIC:
      if ((access_flags & ACC_SYNTHETIC)) {
        return false;
      }
      break;
    case AccessFlag::ANNOTATION:
      if ((access_flags & ACC_ANNOTATION)) {
        return false;
      }
      break;
    case AccessFlag::ENUM:
      if ((access_flags & ACC_ENUM)) {
        return false;
      }
      break;
    }
  }
  return true;
}

template <class DexMember>
bool has_annotation(const DexMember* member, const std::string& annotation) {
  auto annos = member->get_anno_set();
  if (annos != nullptr) {
    auto annotation_regex = proguard_parser::convert_wildcard_type(annotation);
    boost::regex annotation_matcher(annotation_regex);
    for (const auto& anno : annos->get_annotations()) {
      if (boost::regex_match(anno->type()->c_str(), annotation_matcher)) {
        return true;
      }
    }
  }
  return false;
}

bool access_matches(const std::set<AccessFlag>& requiredSet,
                    const std::set<AccessFlag>& requiredUnset,
                    const DexAccessFlags& access_flags) {
  return check_required_access_flags(requiredSet, access_flags) &&
         check_required_unset_access_flags(requiredUnset, access_flags);
}

// From a fully qualified descriptor for a field, exract just the
// name of the field which occurs between the ;. and : characters.
std::string extract_fieldname(std::string qualified_fieldname) {
  auto p = qualified_fieldname.find(";.");
  auto e = qualified_fieldname.find(":");
  return qualified_fieldname.substr(p + 2, e - p - 2);
}

void keep_fields(const ProguardMap& proguard_map,
                 std::list<DexField*> fields,
                 const redex::KeepSpec& keep_rule) {
  auto fieldSpecifications = keep_rule.class_spec.fieldSpecifications;
  for (auto field : fields) {
    for (const auto& fieldSpecification : fieldSpecifications) {
      if (fieldSpecification.annotationType != "") {
        if (!has_annotation(field, fieldSpecification.annotationType)) {
          continue;
        }
      }
      auto pg_name = proguard_name(field);
      auto qualified_name = field->get_deobfuscated_name();
      auto field_name = extract_fieldname(qualified_name);
      // Check for a wildcard match for any field.
      if (fieldSpecification.name == "") {
        TRACE(PGR,
              8,
              "====> Got wildcard field match against %s\n",
              field_name.c_str());
        field->rstate.set_keep();
      } else {
        // Check to see if the field names match. We do not need to
        // check the types for fields since they can't be overloaded.
        TRACE(PGR,
              8,
              "====> Comparing %s vs. %s\n",
              fieldSpecification.name.c_str(),
              field_name.c_str());
        boost::regex fieldname_regex(
            proguard_parser::form_member_regex(fieldSpecification.name));
        if (boost::regex_match(field_name, fieldname_regex)) {
          TRACE(
              PGR, 8, "====> Got fieldname match for %s\n", field_name.c_str());
          field->rstate.set_keep();
          apply_keep_modifiers(keep_rule, field);
        }
      }
    }
  }
}

void apply_field_keeps(const ProguardMap& proguard_map,
                       DexClass* cls,
                       const redex::KeepSpec& keep_rule) {
  keep_fields(proguard_map, cls->get_ifields(), keep_rule);
  keep_fields(proguard_map, cls->get_sfields(), keep_rule);
}

void keep_methods(const redex::KeepSpec& keep_rule,
                  const redex::MemberSpecification& methodSpecification,
                  const ProguardMap& proguard_map,
                  std::list<DexMethod*> methods,
                  const boost::regex& method_regex) {
  for (const auto& method : methods) {
    if (methodSpecification.annotationType != "") {
      if (!has_annotation(method, methodSpecification.annotationType)) {
        continue;
      }
    }
    auto pg_name = proguard_name(method).c_str();
    auto qualified_name = proguard_map.deobfuscate_method(pg_name);
    TRACE(PGR,
          8,
          "====> Checking keeps for method %s | %s | %s\n",
          method->c_str(),
          pg_name,
          qualified_name.c_str());
    if (boost::regex_match(qualified_name.c_str(), method_regex)) {
      TRACE(PGR,
            8,
            "======> Match found, setting keep for %s.\n",
            qualified_name.c_str());
      method->rstate.set_keep();
      apply_keep_modifiers(keep_rule, method);
    }
  }
}

void apply_method_keeps(const ProguardMap& proguard_map,
                        DexClass* cls,
                        const redex::KeepSpec& keep_rule) {
  auto classname = keep_rule.class_spec.className;
  auto methodSpecifications = keep_rule.class_spec.methodSpecifications;
  for (const auto& method_spec : methodSpecifications) {
    auto descriptor = proguard_parser::convert_wildcard_type(classname);
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    auto qualified_method_name =
        desc_regex + "\\." +
        proguard_parser::form_member_regex(method_spec.name) +
        proguard_parser::form_type_regex(method_spec.descriptor);
    TRACE(PGR,
          8,
          "====> Method match against regex method: %s.%s%s\n",
          classname.c_str(),
          method_spec.name.c_str(),
          method_spec.descriptor.c_str());
    TRACE(PGR, 8, "====> Using regex %s\n", qualified_method_name.c_str());
    boost::regex method_regex(qualified_method_name);
    keep_methods(keep_rule,
                 method_spec,
                 proguard_map,
                 cls->get_vmethods(),
                 method_regex);
    keep_methods(keep_rule,
                 method_spec,
                 proguard_map,
                 cls->get_dmethods(),
                 method_regex);
  }
}

void process_proguard_rules(const ProguardConfiguration& pg_config,
                            const ProguardMap& proguard_map,
                            Scope& classes) {
  // Process each keep rule.
  for (const auto& keep_rule : pg_config.keep_rules) {
    // Form a regex for matching against classes in the scope.
    TRACE(PGR,
          8,
          "Processing keep rule for class %s\n",
          keep_rule.class_spec.className.c_str());
    if (keep_rule.class_spec.annotationType != "") {
      TRACE(PGR,
            8,
            "Using annotation type %s\n",
            keep_rule.class_spec.annotationType.c_str());
    }
    auto descriptor =
        proguard_parser::convert_wildcard_type(keep_rule.class_spec.className);
    TRACE(PGR, 8, "==> Descriptor: %s\n", descriptor.c_str());
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(desc_regex);
    // Iterate over each class and process the ones that match this rule.
    for (const auto& cls : classes) {
      // Check to see if we need to match an annotation type.
      if (keep_rule.class_spec.annotationType != "") {
        if (!has_annotation(cls, keep_rule.class_spec.annotationType)) {
          continue;
        }
      }
      auto cname = cls->get_type()->get_name()->c_str();
      auto deob_name = cls->get_deobfuscated_name();
      TRACE(PGR,
            8,
            "==> Examining class %s deobfu: %s\n",
            cname,
            deob_name.c_str());
      if (boost::regex_match(deob_name, matcher)) {
        TRACE(PGR, 8, "==> Got name match for class %s\n", deob_name.c_str());
        if (access_matches(keep_rule.class_spec.setAccessFlags,
                           keep_rule.class_spec.unsetAccessFlags,
                           cls->get_access())) {
          TRACE(PGR, 8, "==> Also got access match\n");
          TRACE(PGR,
                8,
                "====> Setting keep for class %s\n",
                cls->get_deobfuscated_name().c_str());
          cls->rstate.set_keep();
          // Apply the keep option modifiers.
          apply_keep_modifiers(keep_rule, cls);
          // Apply any field-level keep specifications.
          apply_field_keeps(proguard_map, cls, keep_rule);
          // Apply any method-level keep specifications.
          apply_method_keeps(proguard_map, cls, keep_rule);

        } else {
          TRACE(PGR, 8, "==> Failed access match.\n");
        }
      }
    }
  }
}

} // namespace redex
