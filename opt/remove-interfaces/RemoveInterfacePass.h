/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ClassHierarchy.h"
#include "DeterministicContainers.h"
#include "Pass.h"

using TypeSet = std::set<const DexType*, dextypes_comparator>;
class TypeSystem;

/**
 * The motivation of this pass is to remove a hierarhcy of interfaces extending
 * each others. The removal of the interfaces simplifies the type system and
 * enables additional type system level optimizations.
 *
 * We remove each interface by replacing each invoke-interface site with a
 * generated dispatch stub that models the interface call semantic at bytecode
 * level. After that we remove existing references to them from the implementors
 * and remove them completely. We start at the leaf level of the interface
 * hierarchy.
 * After removing the leaf level, we iteratively apply the same
 * transformation to the now newly formed leaf level again and again until all
 * interfaces are removed.
 *
 * Please refer to the instrumentation test config
 * 'test/instr/remove-interface.config' for examples.
 */
class RemoveInterfacePass : public Pass {

 public:
  RemoveInterfacePass() : Pass("RemoveInterfacePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
    };
  }

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<DexType*> m_interface_roots;
  std::vector<DexType*> m_excluded_interfaces;
  DexType* m_interface_dispatch_anno;
  size_t m_total_num_interface = 0;
  size_t m_num_interface_removed = 0;
  size_t m_num_interface_excluded = 0;
  UnorderedSet<const DexType*> m_removed_interfaces;
  bool m_include_primary_dex = false;
  bool m_keep_debug_info = false;
  UnorderedMap<size_t, size_t> m_dispatch_stats;

  void remove_interfaces_for_root(const Scope& scope,
                                  const DexStoresVector& stores,
                                  const DexType* root,
                                  const TypeSystem& type_system);
  TypeSet remove_leaf_interfaces(const Scope& scope,
                                 const DexType* root,
                                 const TypeSet& interfaces,
                                 const TypeSystem& type_system);
  bool is_leaf(const TypeSystem& type_system, const DexType* intf);
  void remove_inheritance(const Scope& scope,
                          const TypeSystem& type_system,
                          const TypeSet& interfaces);
};
