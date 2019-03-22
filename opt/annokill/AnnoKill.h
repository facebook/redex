/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

class AnnoKill {
 public:
  using AnnoSet = std::unordered_set<DexType*>;
  using AnnoNames = std::vector<std::string>;

  struct AnnoKillStats {
    size_t annotations;
    size_t annotations_killed;
    size_t class_asets;
    size_t class_asets_cleared;
    size_t method_asets;
    size_t method_asets_cleared;
    size_t method_param_asets;
    size_t method_param_asets_cleared;
    size_t field_asets;
    size_t field_asets_cleared;
    size_t visibility_build_count;
    size_t visibility_runtime_count;
    size_t visibility_system_count;
    size_t signatures_killed;

    AnnoKillStats() { memset(this, 0, sizeof(AnnoKillStats)); }
  };

  AnnoKill(Scope& scope,
           bool only_force_kill,
           bool kill_bad_signatures,
           const AnnoNames& keep,
           const AnnoNames& kill,
           const AnnoNames& force_kill,
           const std::unordered_map<std::string, std::vector<std::string>>& class_hierarchy_keep_annos,
           const std::unordered_map<std::string, std::vector<std::string>>& annotated_keep_annos
           );

  bool kill_annotations();
  std::unordered_set<const DexType*> build_anno_keep(DexAnnotationSet* aset);
  bool should_kill_bad_signature(DexAnnotation* da);
  AnnoKillStats get_stats() const { return m_stats; }

 private:
  // Gets the set of all annotations referenced in code
  // either by the use of SomeClass.class, as a parameter of a method
  // call or if the annotation is a field of a class.
  AnnoSet get_referenced_annos();

  // Retrieves the list of annotation instances that match the given set
  // of annotation types to be removed.
  AnnoSet get_removable_annotation_instances();

  void cleanup_aset(
    DexAnnotationSet* aset,
    const AnnoSet& referenced_annos,
    const std::unordered_set<const DexType*>& keep_annos = std::unordered_set<const DexType*>{});
  void count_annotation(const DexAnnotation* da);

  Scope& m_scope;
  bool m_only_force_kill;
  bool m_kill_bad_signatures;
  AnnoSet m_kill;
  AnnoSet m_force_kill;
  AnnoSet m_keep;
  AnnoKillStats m_stats;

  std::map<std::string, size_t> m_build_anno_map;
  std::map<std::string, size_t> m_runtime_anno_map;
  std::map<std::string, size_t> m_system_anno_map;
  std::unordered_map<const DexType*, std::unordered_set<const DexType*>> m_anno_class_hierarchy_keep;
  std::unordered_map<const DexType*, std::unordered_set<const DexType*>> m_annotated_keep_annos;
};

class AnnoKillPass : public Pass {
 public:
  AnnoKillPass() : Pass("AnnoKillPass") {}
  explicit AnnoKillPass(const char* name) : Pass(name) {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("keep_annos", {}, m_keep_annos);
    jw.get("kill_annos", {}, m_kill_annos);
    jw.get("force_kill_annos", {}, m_force_kill_annos);
    jw.get("kill_bad_signatures", false, m_kill_bad_signatures);
    std::unordered_map<std::string, std::vector<std::string>> dflt;
    jw.get("class_hierarchy_keep_annos", dflt, m_class_hierarchy_keep_annos);
    jw.get("annotated_keep_annos", dflt, m_annotated_keep_annos);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual bool only_force_kill() const { return false; }

 private:
  std::vector<std::string> m_keep_annos;
  std::vector<std::string> m_kill_annos;
  std::vector<std::string> m_force_kill_annos;
  std::unordered_map<std::string, std::vector<std::string>> m_class_hierarchy_keep_annos;
  std::unordered_map<std::string, std::vector<std::string>> m_annotated_keep_annos;
  bool m_kill_bad_signatures;
};
