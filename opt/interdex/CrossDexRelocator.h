/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "DexStructure.h"
#include "TypeSystem.h"

namespace interdex {

struct CrossDexRelocatorStats {
  size_t classes_added_for_relocated_methods{0};
  size_t relocatable_static_methods{0};
  size_t relocatable_non_static_direct_methods{0};
  size_t relocatable_virtual_methods{0};
  size_t relocated_static_methods{0};
  size_t relocated_non_static_direct_methods{0};
  size_t relocated_virtual_methods{0};
};

struct CrossDexRelocatorConfig {
  bool relocate_static_methods{false};
  bool relocate_non_static_direct_methods{false};
  bool relocate_virtual_methods{false};
  size_t max_relocated_methods_per_class{200};
};

class CrossDexRelocator {
 public:
  CrossDexRelocator(const CrossDexRelocatorConfig& config,
                    const Scope& original_scope,
                    DexesStructure& dexes_structure)
      : m_config(config),
        m_type_system(original_scope),
        m_dexes_structure(dexes_structure) {}

  // Analyze given class, and relocate eligible methods to separate classes.
  void relocate_methods(DexClass* cls,
                        std::vector<DexClass*>& relocated_classes);

  // Indicate that a given class was just emitted into the current dex.
  void add_to_current_dex(DexClass* cls);

  // Indicate that the dex has overflown, and we are now filling up another dex.
  void current_dex_overflowed();

  // After all classes have been emitted, give us a chance to do some cleanup
  // work across the final scope.
  void cleanup(const Scope& final_scope);

  const CrossDexRelocatorConfig& get_config() const { return m_config; }
  const CrossDexRelocatorStats& stats() const { return m_stats; }

 private:
  enum RelocatedMethodKind {
    Static,
    NonStaticDirect,
    Virtual,
  };
  struct RelocatedMethodInfo {
    RelocatedMethodKind kind;
    DexMethod* method;
    DexClass* source_class;
    int api_level;
    bool is_dependent_non_static_direct{false};
  };
  struct RelocatedTargetClassInfo {
    DexClass* cls;
    size_t size{0}; // number of methods
  };

  std::string create_new_type_name(RelocatedMethodKind kind);

  void gather_possibly_relocatable_methods(
      DexClass* cls, std::vector<DexMethod*>& possibly_relocatable_methods);

  // We track dependencies on invoked direct methods that get relocated
  // themselves
  bool handle_invoked_direct_methods_that_prevent_relocation(
      DexMethod* m,
      std::unordered_map<DexMethod*, DexClass*>& relocated_methods);

  // Undoing a previous relocation, in case the method ends up in the dex of its
  // source class
  void re_relocate_method(const RelocatedMethodInfo& info,
                          DexClass* target_class);

  std::unordered_map<DexClass*, RelocatedMethodInfo> m_relocated_method_infos;
  std::unordered_map<int32_t, RelocatedTargetClassInfo>
      m_relocated_target_classes;
  std::unordered_map<DexClass*, std::vector<RelocatedMethodInfo>>
      m_source_class_to_relocated_method_infos_map;
  std::unordered_set<DexClass*> m_classes_in_current_dex;
  std::unordered_set<DexMethod*> m_relocated_non_static_methods;
  size_t m_next_method_id{0};
  CrossDexRelocatorStats m_stats;
  const CrossDexRelocatorConfig m_config;
  TypeSystem m_type_system;
  DexesStructure& m_dexes_structure;
};

} // namespace interdex
