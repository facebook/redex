/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardObfuscationTest.h"
#include "Show.h"
#include "Walkers.h"

ProguardObfuscationTest::ProguardObfuscationTest(const char* dexfile,
                                                 const char* mapping_file)
    : proguard_map(std::string(mapping_file)) {
  dexen.emplace_back(load_classes_from_dex(dexfile));
}

bool ProguardObfuscationTest::configure_proguard(
    const char* configuration_file) {
  keep_rules::ProguardConfiguration pg_config;
  keep_rules::proguard_parser::parse_file(configuration_file, &pg_config);

  if (!pg_config.ok) {
    return false;
  }
  Scope scope = build_class_scope(dexen);
  // We aren't loading any external jars for this test, so external_classes is
  // empty
  Scope external_classes;
  apply_deobfuscated_names(dexen, proguard_map);
  process_proguard_rules(
      proguard_map, scope, external_classes, pg_config, true);
  return true;
}

DexClass* ProguardObfuscationTest::find_class_named(const std::string& name) {
  DexClasses& classes = dexen.front();
  auto mapped_search_name = std::string(proguard_map.translate_class(name));
  auto it = std::find_if(
      classes.begin(), classes.end(), [&mapped_search_name](DexClass* cls) {
        return mapped_search_name == std::string(cls->c_str());
      });
  if (it == classes.end()) {
    return nullptr;
  } else {
    return *it;
  }
}

bool ProguardObfuscationTest::field_found(const std::vector<DexField*>& fields,
                                          const std::string& name) {
  auto it = std::find_if(fields.begin(), fields.end(), [&](DexField* field) {
    auto deobfuscated_name = proguard_map.deobfuscate_field(show(field));
    return (name == std::string(field->c_str()) || name == deobfuscated_name ||
            name == show(field)) &&
           deobfuscated_name == show(field);
  });
  return it != fields.end();
}

int ProguardObfuscationTest::method_is_renamed_helper(
    const std::vector<DexMethod*>& methods, const std::string& name) {
  for (const auto& method : methods) {
    auto deobfuscated_name = proguard_map.deobfuscate_method(show(method));
    if (name == std::string(method->c_str()) || name == deobfuscated_name) {
      return deobfuscated_name != show(method);
    }
  }
  return -1;
}

bool ProguardObfuscationTest::method_is_renamed(const DexClass* cls,
                                                const std::string& name) {
  auto is_renamed_vmeth = method_is_renamed_helper(cls->get_vmethods(), name);
  auto is_renamed_dmeth = method_is_renamed_helper(cls->get_dmethods(), name);
  // If either of them found the method to be renamed, return that, otherwise
  // if neither found the method, assume it's renamed
  return is_renamed_dmeth == 1 || is_renamed_vmeth == 1 ||
         (is_renamed_dmeth == -1 && is_renamed_vmeth == -1);
}

bool ProguardObfuscationTest::refs_to_field_found(const std::string& name) {
  bool res = false;
  DexClasses& classes(dexen.front());
  walk::opcodes(classes,
                [](DexMethod*) { return true; },
                [&](DexMethod* method, IRInstruction* instr) {
                  if (!opcode::is_an_ifield_op(instr->opcode())) return;
                  DexFieldRef* field_ref = instr->get_field();
                  if (field_ref->is_def()) return;

                  res |= show(field_ref) == name;
                });
  return res;
}
