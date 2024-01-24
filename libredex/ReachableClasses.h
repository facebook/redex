/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "DexClass.h"
#include "DexUtil.h"
#include "JsonWrapper.h"

struct ReachableClassesConfig {
  std::string apk_dir;
  std::vector<std::string> reflected_package_names;
  std::unordered_set<std::string> prune_unexported_components;
  bool compute_xml_reachability = true;
  bool analyze_native_lib_reachability = true;
  std::vector<std::string> keep_methods;
  std::vector<std::string> json_serde_supercls;
  std::vector<std::string> fbjni_json_files;

  ReachableClassesConfig() {}

  explicit ReachableClassesConfig(const JsonWrapper& config) {
    config.get("apk_dir", "", apk_dir);
    config.get("keep_packages", {}, reflected_package_names);

    config.get("compute_xml_reachability", true, compute_xml_reachability);
    config.get("prune_unexported_components", {}, prune_unexported_components);
    config.get("analyze_native_lib_reachability", true,
               analyze_native_lib_reachability);

    config.get("keep_methods", {}, keep_methods);
    config.get("json_serde_supercls", {}, json_serde_supercls);
    config.get("fbjni_json_files", {}, fbjni_json_files);
  }
};

void init_reachable_classes(const Scope& scope,
                            const ReachableClassesConfig& config);

void recompute_reachable_from_xml_layouts(const Scope& scope,
                                          const std::string& apk_dir);

template <class DexMember>
inline bool can_delete(DexMember* member) {
  return !member->is_external() && member->rstate.can_delete();
}

template <class DexMember>
inline bool root(DexMember* member) {
  return !can_delete(member);
}

template <class DexMember>
inline bool can_rename(DexMember* member) {
  return !member->is_external() && member->rstate.can_rename();
}

template <class DexMember>
inline bool can_rename_if_also_renaming_xml(DexMember* member) {
  return member->rstate.can_rename_if_also_renaming_xml();
}

inline bool is_serde(const DexClass* member) {
  return member->rstate.is_serde();
}

inline bool marked_by_string(const DexClass* member) {
  return member->rstate.is_referenced_by_string();
}

template <class DexMember>
inline bool assumenosideeffects(DexMember* member) {
  return member->rstate.assumenosideeffects();
}
