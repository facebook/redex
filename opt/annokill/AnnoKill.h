/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "ConcurrentContainers.h"

class DexAnnotation;
class DexAnnotationSet;

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

    AnnoKillStats& operator+=(const AnnoKillStats& rhs) {
      annotations += rhs.annotations;
      annotations_killed += rhs.annotations_killed;
      class_asets += rhs.class_asets;
      class_asets_cleared += rhs.class_asets_cleared;
      method_asets += rhs.method_asets;
      method_asets_cleared += rhs.method_asets_cleared;
      method_param_asets += rhs.method_param_asets;
      method_param_asets_cleared += rhs.method_param_asets_cleared;
      field_asets += rhs.field_asets;
      field_asets_cleared += rhs.field_asets_cleared;
      visibility_build_count += rhs.visibility_build_count;
      visibility_runtime_count += rhs.visibility_runtime_count;
      visibility_system_count += rhs.visibility_system_count;
      signatures_killed += rhs.signatures_killed;
      return *this;
    }
  };

  AnnoKill(Scope& scope,
           bool only_force_kill,
           bool kill_bad_signatures,
           const AnnoNames& keep,
           const AnnoNames& kill,
           const AnnoNames& force_kill,
           const std::unordered_map<std::string, std::vector<std::string>>&
               class_hierarchy_keep_annos,
           const std::unordered_map<std::string, std::vector<std::string>>&
               annotated_keep_annos);

  bool kill_annotations();
  std::unordered_set<const DexType*> build_anno_keep(
      DexAnnotationSet* aset) const;
  bool should_kill_bad_signature(DexAnnotation* da) const;
  AnnoKillStats get_stats() const { return m_stats; }

 private:
  // Gets the set of all annotations referenced in code
  // either by the use of SomeClass.class, as a parameter of a method
  // call or if the annotation is a field of a class.
  AnnoSet get_referenced_annos();

  // Retrieves the list of annotation instances that match the given set
  // of annotation types to be removed.
  AnnoSet get_removable_annotation_instances();

  void cleanup_aset(DexAnnotationSet* aset,
                    const AnnoSet& referenced_annos,
                    AnnoKillStats& stats,
                    const std::unordered_set<const DexType*>& keep_annos =
                        std::unordered_set<const DexType*>{}) const;
  void count_annotation(const DexAnnotation* da, AnnoKillStats& stats) const;

  Scope& m_scope;
  std::unordered_set<DexClass*> m_scope_set;
  const bool m_only_force_kill;
  const bool m_kill_bad_signatures;
  AnnoSet m_kill;
  AnnoSet m_force_kill;
  AnnoSet m_keep;
  AnnoKillStats m_stats;

  mutable ConcurrentMap<std::string_view, size_t> m_build_anno_map;
  mutable ConcurrentMap<std::string_view, size_t> m_runtime_anno_map;
  mutable ConcurrentMap<std::string_view, size_t> m_system_anno_map;
  std::unordered_map<const DexType*, std::unordered_set<const DexType*>>
      m_anno_class_hierarchy_keep;
  std::unordered_map<const DexType*, std::unordered_set<const DexType*>>
      m_annotated_keep_annos;
};

class AnnoKillPass : public Pass {
 public:
  AnnoKillPass() : Pass("AnnoKillPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  explicit AnnoKillPass(const std::string& name) : Pass(name) {}

  void bind_config() override {
    bind("keep_annos", {}, m_keep_annos);
    bind("kill_annos", {}, m_kill_annos);
    bind("force_kill_annos", {}, m_force_kill_annos);
    bind("kill_bad_signatures", false, m_kill_bad_signatures);
    bind("class_hierarchy_keep_annos", {}, m_class_hierarchy_keep_annos);
    bind("annotated_keep_annos", {}, m_annotated_keep_annos);
    bind("only_force_kill", false, m_only_force_kill);
  }

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  std::unique_ptr<Pass> clone(const std::string& new_name) const override {
    return std::make_unique<AnnoKillPass>(new_name);
  }

 private:
  std::vector<std::string> m_keep_annos;
  std::vector<std::string> m_kill_annos;
  std::vector<std::string> m_force_kill_annos;
  std::unordered_map<std::string, std::vector<std::string>>
      m_class_hierarchy_keep_annos;
  std::unordered_map<std::string, std::vector<std::string>>
      m_annotated_keep_annos;
  bool m_kill_bad_signatures;

 protected:
  bool m_only_force_kill;
};
