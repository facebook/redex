/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <iostream>
#include <string>

#include "DexAccess.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "keeprules.h"

namespace redex {

std::string dextype_from_dotname(std::string dotname) {
  std::string buf;
  buf.reserve(dotname.size() + 2);
  buf += 'L';
  buf += dotname;
  buf += ';';
  std::replace(buf.begin(), buf.end(), '.', '/');
  return buf;
}

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
void keep_fields(ProguardMap* proguard_map,
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
        TRACE(PGR, 8, "Comparing %s vs. %s\n", fieldSpecification.name.c_str(), field_name.c_str());
        if (fieldSpecification.name == field_name) {
          TRACE(PGR, 8, "====> Got filedname match for %s\n", field_name.c_str());
          field->rstate.set_keep();
        }
      }
    }
  }
}

// Currently only field level keeps of wildcard * specifications
// and literal identifier matches (but no wildcards yet).
void apply_field_keeps(ProguardMap* proguard_map,
                       DexClass* cls,
                       const std::vector<MemberSpecification>& fieldSpecifications) {
  if (fieldSpecifications.empty()) {
    return;
  }
  keep_fields(proguard_map, cls->get_ifields(), fieldSpecifications);
  keep_fields(proguard_map, cls->get_sfields(), fieldSpecifications);
}

void process_proguard_rules(const ProguardConfiguration& pg_config,
                            ProguardMap* proguard_map,
                            Scope& classes) {
  for (const auto& cls : classes) {
    auto cname = cls->get_type()->get_name()->c_str();
    auto cls_len = strlen(cname);
    TRACE(PGR, 8, "Examining class %s deobfu: %s\n", cname, cls->get_deobfuscated_name().c_str());
    for (const auto& k : pg_config.keep_rules) {
      auto keep_name = dextype_from_dotname(k.class_spec.className);
      std::string translated_keep_name =
          proguard_map->translate_class(keep_name);
      TRACE(PGR,
            8,
            "==> Checking against keep rule for %s (%s)\n",
            keep_name.c_str(),
            translated_keep_name.c_str());
      if (type_matches(translated_keep_name.c_str(),
                       cname,
                       translated_keep_name.size(),
                       cls_len) &&
          access_matches(k.class_spec.setAccessFlags,
                         k.class_spec.unsetAccessFlags,
                         cls->get_access())) {
        TRACE(PGR, 8, "Setting keep for class %s\n", cls->get_deobfuscated_name().c_str());
        cls->rstate.set_keep();
        // Apply the keep option modifiers.
        apply_keep_modifiers(k, cls);
        // Apply any field-level keep specifications.
        apply_field_keeps(proguard_map, cls, k.class_spec.fieldSpecifications);
      }
    }
  }
}

} // namespace redex
