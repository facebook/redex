/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

class RenameClassesPassV2 : public Pass {
 public:
  RenameClassesPassV2() : Pass("RenameClassesPassV2") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("class_rename", "", m_path);
    pc.get("rename_annotations", false, m_rename_annotations);
    pc.get("dont_rename_hierarchies", {}, m_dont_rename_hierarchies);
    pc.get("dont_rename_annotated", {}, m_dont_rename_annotated);
    std::vector<std::string> dont_rename_specific;
    pc.get("dont_rename_specific", {}, dont_rename_specific);
    m_dont_rename_specific.insert(dont_rename_specific.begin(), dont_rename_specific.end());
  }

  virtual void run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) override;

 private:

    void build_dont_rename_resources(PassManager& mgr, std::set<std::string>& dont_rename_resources);
    void build_dont_rename_class_for_name_literals(Scope& scope, std::set<std::string>& dont_rename_class_for_name_literals);
    void build_dont_rename_canaries(Scope& scope,std::set<std::string>& dont_rename_canaries);
    void build_dont_rename_hierarchies(Scope& scope, std::unordered_map<const DexType*, std::string>& dont_rename_hierarchies);
    void build_dont_rename_native_bindings(Scope& scope, std::set<DexType*>& dont_rename_native_bindings);
    void build_dont_rename_annotated(std::set<DexType*>& dont_rename_annotated);
    void rename_classes(Scope& scope, const std::string& path, bool rename_annotations, PassManager& mgr);

  std::string m_path;
  bool m_rename_annotations;
  std::vector<std::string> m_dont_rename_hierarchies;
  std::vector<std::string> m_dont_rename_annotated;
  std::unordered_set<std::string> m_dont_rename_specific;
};
