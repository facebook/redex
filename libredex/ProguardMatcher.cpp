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

// From a fully qualified descriptor for a field, exract just the
// name of the field which occurs between the ;. and : characters.
std::string extract_fieldname(std::string qualified_fieldname) {
  auto p = qualified_fieldname.find(";.");
  auto e = qualified_fieldname.find(":");
  return qualified_fieldname.substr(p+2, e-p-2);
}

// Currently only field level keeps of wildcard * specifications
// and literal identifier matches (but no wildcards yet).
void apply_field_keeps(ProguardMap* proguard_map, DexClass* cls,
                       std::vector<MemberSpecification> fieldSpecifications) {
  if (fieldSpecifications.empty()) {
    return;
  }
  for (auto field : cls->get_ifields()) {
    for (const auto& fieldSpecification : fieldSpecifications) {
      auto pg_name = proguard_name(field);
      auto qualified_name = field->get_deobfuscated_name();
      auto field_name = extract_fieldname(qualified_name);
      // Check for a wildcard match for any field.
      if (fieldSpecification.name == "") {
        field->rstate.set_keep();
      } else {
        // Check to see if the field names match. We do not need to
        // check the types for fields since they can't be overloaded.
        if (fieldSpecification.name == field_name) {
          field->rstate.set_keep();
        }
      }
    }
  }
}

void process_proguard_rules(const ProguardConfiguration& pg_config,
                            ProguardMap* proguard_map,
                            Scope& classes) {
  for (const auto& cls : classes) {
    auto cname = cls->get_type()->get_name()->c_str();
    auto cls_len = strlen(cname);
    TRACE(PGR, 8, "Examining class %s\n", cname);
    for (const auto& k : pg_config.keep_rules) {
		  auto keep_name = dextype_from_dotname(k.class_spec.className);
      std::string translated_keep_name = proguard_map->translate_class(keep_name);
      TRACE(PGR,
            8,
            "==> Checking against keep rule for %s (%s)\n",
            keep_name.c_str(), translated_keep_name.c_str());
      if (type_matches(translated_keep_name.c_str(),
                       cname,
                       translated_keep_name.size(),
                       cls_len)) {
        TRACE(PGR, 8, "Setting keep for %s\n", cname);
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
