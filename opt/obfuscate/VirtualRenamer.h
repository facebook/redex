/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "ObfuscateUtils.h"
#include "Devirtualizer.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>

class MethodLinkManager {
public:
  // class -> interfaces implemented in its hierarchy
  std::unordered_map<const DexType*,
      std::unordered_set<const DexType*>> class_interfaces;
  // methods that implement an interface
  std::unordered_map<DexString*,
      std::unordered_map<DexProto*,
          std::unordered_set<DexMethod*>>> interface_methods;
  // class -> set of all public methods in its hierarchy
  std::unordered_map<const DexType*,
      std::unordered_set<DexMethod*>> class_conflict_set;
  // for recursion - set of all public methods in any superclasses
  std::unordered_set<DexMethod*> parent_conflict_set;
  // index with name, proto
  // reason for the vector: we might have two elements with the same signature
  // as we merge the maps going up the tree, but we don't necessarily want
  // to link these elements
  std::unordered_map<
      DexString*,
      std::unordered_map<DexProto*, std::vector<MethodNameWrapper*>>>
      method_map;

  MethodLinkManager() {}
};

class MethodLinkInfo {
public:
  // Map from class to the interfaces it could implement (sensitive to
  // hierarchies such that it includes interfaces anywhere in the hierarchy)
  std::unordered_map<const DexType*,
      std::unordered_set<const DexType*>> class_interfaces;
  // Map from an interface to the set of names of methods of the interface
  std::unordered_map<const DexType*,
      std::unordered_set<DexMethod*>> intf_conflict_set;
  // The name manager including all of the "link" information for vmethods
  DexMethodManager name_manager;

  MethodLinkInfo(
      std::unordered_map<const DexType*,
          std::unordered_set<const DexType*>>& class_interfaces,
      DexMethodManager& name_mapping) :
          class_interfaces(std::move(class_interfaces)),
          name_manager(std::move(name_mapping)) { }
};

MethodLinkInfo link_methods(Scope& scope);

struct VirtualRenamer {
  const std::vector<DexClass*>& m_scope;
  ClassHierarchy class_hierarchy;
  SignatureMap methods;

  explicit VirtualRenamer(const std::vector<DexClass*>& scope);

  //TODO: those will go once we fix the dependency in the obfuscator
  MethodLinkInfo link_methods();
  DexMethodManager name_manager;
  bool link_methods_helper(
      const DexType* parent, const TypeSet& children,
      MethodLinkManager& links);
  void mark_methods_renamable(const DexType* cls);
};
