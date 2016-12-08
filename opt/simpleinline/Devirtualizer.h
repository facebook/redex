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
#include <vector>
#include <unordered_map>
#include <unordered_set>

/**
 * Given a scope find all virtual method that can be devirtualized.
 * That is, methods that have a unique definition in the vmethods across
 * an hierarchy. Basically all methods that are virtual because of visibility
 * (public, package and protected) and not because they need to be virtual.
 */
std::vector<DexMethod*> devirtualize(std::vector<DexClass*>& scope);


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

MethodLinkInfo link_methods(std::vector<DexClass*>& scope);
