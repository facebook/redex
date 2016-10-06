/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ProguardObfuscationTest.h"

ProguardObfuscationTest::ProguardObfuscationTest(
    const char* dexfile,
    const char* mapping_file) :
  proguard_map(std::string(mapping_file)) {
  dexen.emplace_back(load_classes_from_dex(dexfile));
}

bool ProguardObfuscationTest::configure_proguard(
    const char* configuration_file) {
  redex::ProguardConfiguration pg_config;
  redex::proguard_parser::parse_file(configuration_file, &pg_config);

  if (!pg_config.ok) {
    return false;
  }
  Scope scope = build_class_scope(dexen);
  apply_deobfuscated_names(dexen, proguard_map);
  process_proguard_rules(proguard_map, pg_config, scope);
  return true;
}

DexClass* ProguardObfuscationTest::find_class_named(
    const std::string& name) {
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

bool ProguardObfuscationTest::field_is_renamed(
    const std::list<DexField*>& fields,
    const std::string& name) {
  for (const auto& field : fields) {
    auto deobfuscated_name = proguard_map.deobfuscate_field(proguard_name(field));
    if (name == std::string(field->c_str()) || (name == deobfuscated_name)) {
      return deobfuscated_name != proguard_name(field);
    }
  }
  return false;
}

bool ProguardObfuscationTest::method_is_renamed_helper(
    const std::list<DexMethod*>& methods,
    const std::string& name) {
  for (const auto& method : methods) {
    auto deobfuscated_name = method->get_deobfuscated_name();
    if (name == std::string(method->c_str()) ||
      name == deobfuscated_name) {
      return deobfuscated_name != proguard_name(method);
    }
  }
  return false;
}

bool ProguardObfuscationTest::method_is_renamed(
    const DexClass* cls, const std::string& name) {
  return method_is_renamed_helper(cls->get_vmethods(), name) ||
    method_is_renamed_helper(cls->get_dmethods(), name);
}
