/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveUnreachable.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "ReachableObjects.h"
#include "Resolver.h"
#include "Show.h"

#include <string>

/**
 * RemoveUnreachable eliminates unreachable classes, methods, and fields by a
 * mark-sweep algorithm. Refer ReachableObjects.cpp for the marking phase.
 */

namespace {

/*
 * Remove unmarked fields from :fields and erase their definitions from
 * g_redex.
 */
void sweep_fields_if_unmarked(
    std::vector<DexField*>& fields,
    const std::unordered_set<const DexFieldRef*>& marked) {
  auto p = [&](DexField* f) {
    if (marked.count(f) == 0) {
      TRACE(RMU, 2, "Removing %s\n", SHOW(f));
      DexField::erase_field(f);
      return true;
    }
    return false;
  };
  fields.erase(std::remove_if(fields.begin(), fields.end(), p), fields.end());
}

/*
 * Remove unmarked classes and methods. This should really erase the classes /
 * methods from g_redex as well, but that will probably result in dangling
 * pointers (at least for DexMethods). We should fix that at some point...
 */
template <class Container, class Marked>
void sweep_if_unmarked(Container& c, const std::unordered_set<Marked>& marked) {
  auto p = [&](const Marked& m) {
    if (marked.count(m) == 0) {
      TRACE(RMU, 2, "Removing %s\n", SHOW(m));
      return true;
    }
    return false;
  };
  c.erase(std::remove_if(c.begin(), c.end(), p), c.end());
}

void sweep(DexStoresVector& stores, ReachableObjects& reachables) {
  for (auto& dex : DexStoreClassesIterator(stores)) {
    sweep_if_unmarked(dex, reachables.marked_classes);
    for (auto const& cls : dex) {
      sweep_fields_if_unmarked(cls->get_ifields(), reachables.marked_fields);
      sweep_fields_if_unmarked(cls->get_sfields(), reachables.marked_fields);
      sweep_if_unmarked(cls->get_dmethods(), reachables.marked_methods);
      sweep_if_unmarked(cls->get_vmethods(), reachables.marked_methods);
    }
  }
}
} // namespace

struct deleted_stats {
  size_t nclasses{0};
  size_t nfields{0};
  size_t nmethods{0};
};

deleted_stats trace_stats(const char* label, DexStoresVector& stores) {
  deleted_stats stats;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    stats.nclasses += dex.size();
    for (auto const& cls : dex) {
      stats.nfields += cls->get_ifields().size();
      stats.nfields += cls->get_sfields().size();
      stats.nmethods += cls->get_dmethods().size();
      stats.nmethods += cls->get_vmethods().size();
    }
  }
  TRACE(RMU,
        1,
        "%s: %lu classes, %lu fields, %lu methods\n",
        label,
        stats.nclasses,
        stats.nfields,
        stats.nmethods);
  return stats;
}

void RemoveUnreachablePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& /*cfg*/,
                                     PassManager& pm) {
  if (pm.no_proguard_rules()) {
    TRACE(RMU,
          1,
          "RemoveUnreachablePass not run because no "
          "ProGuard configuration was provided.");
    return;
  }

  auto load_annos = [](const std::vector<std::string>& list) {
    std::unordered_set<const DexType*> set;
    for (const auto& name : list) {
      const auto type = DexType::get_type(name.c_str());
      if (type != nullptr) {
        set.insert(type);
      }
    }
    return set;
  };

  int num_ignore_check_strings = 0;
  auto reachables =
      compute_reachable_objects(stores,
                                load_annos(m_ignore_string_literals),
                                load_annos(m_ignore_string_literal_annos),
                                load_annos(m_ignore_system_annos),
                                &num_ignore_check_strings);
  deleted_stats before = trace_stats("before", stores);
  sweep(stores, reachables);
  deleted_stats after = trace_stats("after", stores);
  pm.incr_metric("num_ignore_check_strings", num_ignore_check_strings);
  pm.incr_metric("classes_removed", before.nclasses - after.nclasses);
  pm.incr_metric("fields_removed", before.nfields - after.nfields);
  pm.incr_metric("methods_removed", before.nmethods - after.nmethods);
}

static RemoveUnreachablePass s_pass;
