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

void apply_keep_modifiers(const KeepSpec& k, DexClass* cls) {
  if (k.includedescriptorclasses) {
    cls->rstate.set_includedescriptorclasses();
  }
  if (k.allowoptimization) {
    cls->rstate.set_allowoptimization();
  }
  if (k.allowshrinking) {
    cls->rstate.set_allowshrinking();
  }
  if (k.allowobfuscation) {
    cls->rstate.set_allowobfuscation();
  }
}

bool check_required_access_flags(const std::set<AccessFlag>& requiredSet,
                                 const DexAccessFlags& access_flags) {
  for (const AccessFlag& af : requiredSet) {
    switch (af) {
    case AccessFlag::PUBLIC:
      if (!(access_flags & ACC_PUBLIC)) {
        return false;
      }
      break;
    case AccessFlag::PRIVATE:
      if (!(access_flags & ACC_PRIVATE)) {
        return false;
      }
      break;
    case AccessFlag::PROTECTED:
      if (!(access_flags & ACC_PROTECTED)) {
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

// Currently only field level keeps of wildcard * specifications
// and literal identifier matches (but no wildcards yet).
void keep_fields(const ProguardMap* proguard_map,
                 std::list<DexField*> fields,
                 const std::vector<MemberSpecification>& fieldSpecifications) {
  for (auto field : fields) {
    for (const auto& fieldSpecification : fieldSpecifications) {
      auto pg_name = proguard_name(field);
      auto qualified_name = field->get_deobfuscated_name();
      auto field_name = extract_fieldname(qualified_name);
      // Check for a wildcard match for any field.
      if (fieldSpecification.name == "") {
        TRACE(PGR, 8, "====> Got wildcard field match against %s\n", field_name.c_str());
        field->rstate.set_keep();
      } else {
        // Check to see if the field names match. We do not need to
        // check the types for fields since they can't be overloaded.
        TRACE(PGR, 8, "====> Comparing %s vs. %s\n", fieldSpecification.name.c_str(), field_name.c_str());
        boost::regex fieldname_regex(proguard_parser::form_member_regex(fieldSpecification.name));
        if (boost::regex_match(field_name, fieldname_regex)) {
          TRACE(PGR, 8, "====> Got fieldname match for %s\n", field_name.c_str());
          field->rstate.set_keep();
        }
      }
    }
  }
}

// Currently only field level keeps of wildcard * specifications
// and literal identifier matches (but no wildcards yet).
void apply_field_keeps(const ProguardMap* proguard_map,
                       DexClass* cls,
                       const std::vector<MemberSpecification>& fieldSpecifications) {
  if (fieldSpecifications.empty()) {
    return;
  }
  keep_fields(proguard_map, cls->get_ifields(), fieldSpecifications);
  keep_fields(proguard_map, cls->get_sfields(), fieldSpecifications);
}

void keep_methods(const ProguardMap* proguard_map,
                  std::list<DexMethod*> methods,
                  const boost::regex& method_regex) {
  for (const auto& method : methods) {
    auto pg_name = proguard_name(method).c_str();
    auto qualified_name = proguard_map->deobfuscate_method(pg_name);
    TRACE(PGR, 8, "====> Checking keeps for method %s | %s | %s\n", method->c_str(), pg_name, qualified_name.c_str());
    if (boost::regex_match(qualified_name.c_str(), method_regex)) {
      TRACE(PGR, 8, "======> Match found, setting keep for %s.\n", qualified_name.c_str());
      method->rstate.set_keep();
    }
  }
}

void apply_method_keeps(const ProguardMap* proguard_map,
                        DexClass* cls,
                        const std::string classname,
                        const std::vector<MemberSpecification>& methodSpecifications) {
  for (const auto& method_spec : methodSpecifications) {
    auto descriptor = proguard_parser::convert_wildcard_type(classname);
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    auto qualified_method_name = desc_regex + "\\." +
                                 proguard_parser::form_member_regex(method_spec.name) +
                                 proguard_parser::form_type_regex(method_spec.descriptor);
    TRACE(PGR, 8, "====> Method match against regex method: %s.%s%s\n", classname.c_str(), method_spec.name.c_str(), method_spec.descriptor.c_str());
    TRACE(PGR, 8, "====> Using regex %s\n", qualified_method_name.c_str());
    boost::regex method_regex(qualified_method_name);
    keep_methods(proguard_map, cls->get_vmethods(), method_regex);
    keep_methods(proguard_map, cls->get_dmethods(), method_regex);
  }
}

void process_proguard_rules(const ProguardConfiguration& pg_config,
                            ProguardMap* proguard_map,
                            Scope& classes) {
  // Process each keep rule.
  for (const auto& keep_rule : pg_config.keep_rules) {
    // Form a regex for matching against classes in the scope.
    TRACE(PGR, 8, "Processing keep rule for class %s\n", keep_rule.class_spec.className.c_str());
    auto descriptor = proguard_parser::convert_wildcard_type(keep_rule.class_spec.className);
    TRACE(PGR, 8, "==> Descriptor: %s\n", descriptor.c_str());
    auto desc_regex = proguard_parser::form_type_regex(descriptor);
    boost::regex matcher(desc_regex);
    // Iterate over each class and process the ones that match this rule.
    for (const auto& cls : classes) {
      auto cname = cls->get_type()->get_name()->c_str();
      auto deob_name = cls->get_deobfuscated_name();
      TRACE(PGR, 8, "==> Examining class %s deobfu: %s\n", cname, deob_name.c_str());
      if (boost::regex_match(deob_name, matcher)) {
         TRACE(PGR, 8, "==> Got name match for class %s\n", deob_name.c_str());
         if ( access_matches(keep_rule.class_spec.setAccessFlags,
                         keep_rule.class_spec.unsetAccessFlags,
                         cls->get_access())) {
           TRACE(PGR, 8, "==> Also got access match\n");
        TRACE(PGR, 8, "====> Setting keep for class %s\n", cls->get_deobfuscated_name().c_str());
        cls->rstate.set_keep();
        // Apply the keep option modifiers.
        apply_keep_modifiers(keep_rule, cls);
        // Apply any field-level keep specifications.
        apply_field_keeps(proguard_map, cls, keep_rule.class_spec.fieldSpecifications);
        // Apply any method-level keep specifications.
        apply_method_keeps(proguard_map, cls, keep_rule.class_spec.className, keep_rule.class_spec.methodSpecifications);

        } else {
           TRACE(PGR, 8, "==> Failed access match.\n");
       }
      }
    }
  }
}

} // namespace redex
