/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MergeInterface.h"

#include "ClassHierarchy.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IROpcode.h"
#include "Resolver.h"
#include "Trace.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

using DexClassSet = std::set<DexClass*, dexclasses_comparator>;

// List of classes to List of interfaces they implemented
using ImplementorsToInterfaces = std::map<TypeSet, DexClassSet>;

std::string show(const ImplementorsToInterfaces& input) {
  std::ostringstream ss;
  ss << "============ interface and class map ============\n";
  for (const auto& pair : input) {
    ss << "classes: \n";
    for (const auto& cls : pair.first) {
      ss << "   " << cls << "\n";
    }
    ss << "interfaces: \n";
    for (const auto& cls : pair.second) {
      ss << "   " << cls << "\n";
    }
  }
  return ss.str();
}

std::string show(const std::vector<DexClassSet>& to_merge) {
  std::ostringstream ss;
  ss << "\n============ Interfaces to merge ============\n";
  for (auto it = to_merge.begin(); it != to_merge.end(); ++it) {
    ss << "Interfaces to merge: \n";
    for (const auto& intf : *it) {
      ss << "   " << intf << "\n";
    }
  }
  return ss.str();
}

std::vector<DexClassSet> collect_can_merge(const Scope& scope,
                                           MergeInterfacePass::Metric* metric) {
  // Build the map of interfaces and list of classes that implement
  // the interfaces
  ImplementorsToInterfaces interface_class_map;
  TypeSystem ts(scope);
  std::unordered_set<DexClass*> ifaces;
  // Find interfaces that are not external, can be delete, can be renamed.
  for (auto cls : scope) {
    if (is_interface(cls) && !cls->is_external() && can_delete(cls) &&
        can_rename_if_ignoring_blanket_keepnames(cls)) {
      ifaces.emplace(cls);
    }
  }
  for (const auto& cls : ifaces) {
    auto implementors = ts.get_implementors(cls->get_type());
    interface_class_map[implementors].emplace(cls);
  }
  TRACE(MEINT, 5, SHOW(interface_class_map));

  // Collect interfaces that we need to merge.
  std::vector<DexClassSet> interface_set;
  for (const auto& pair : interface_class_map) {
    if (pair.first.size() > 0 && pair.second.size() > 1) {
      // Consider interfaces with same set of implementors as mergeable.
      interface_set.emplace_back(pair.second);
      metric->interfaces_to_merge += pair.second.size();
    }
  }

  // Remove interface if it is the type of an annotation.
  // TODO(suree404): Merge the interface even though it appears in annotation?
  walk::annotations(scope, [&](DexAnnotation* anno) {
    std::vector<DexType*> types_in_anno;
    anno->gather_types(types_in_anno);
    for (const auto& type : types_in_anno) {
      auto type_cls = type_class(type);
      if (type_cls == nullptr) continue;
      for (auto it = interface_set.begin(); it != interface_set.end(); ++it) {
        if (it->count(type_cls) > 0) {
          it->erase(type_cls);
          ++metric->interfaces_in_annotation;
          break;
        }
      }
    }
  });
  metric->interfaces_to_merge -= metric->interfaces_in_annotation;
  TRACE(MEINT, 4, SHOW(interface_set));
  metric->interfaces_merge_left = interface_set.size();
  return interface_set;
}

} // namespace

void MergeInterfacePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& /*cfg*/,
                                  PassManager& mgr) {
  auto scope = build_class_scope(stores);

  auto can_merge = collect_can_merge(scope, &m_metric);

  mgr.set_metric("num_mergeable_interfaces", m_metric.interfaces_to_merge);
  mgr.set_metric("num_created_interfaces", m_metric.interfaces_merge_left);
  mgr.set_metric("num_interfaces_in_anno_not_merging",
                 m_metric.interfaces_in_annotation);
}

static MergeInterfacePass s_pass;
