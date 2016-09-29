/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AccessMarking.h"

#include <unordered_map>

#include "DexUtil.h"
#include "ReachableClasses.h"

namespace {
size_t mark_classes_final(const DexStoresVector& stores) {
  size_t n_classes_finalized = 0;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    for (auto const& cls : dex) {
      if (is_seed(cls) || is_abstract(cls) || is_final(cls)) continue;
      auto const& children = get_children(cls->get_type());
      if (children.empty()) {
        TRACE(ACCESS, 2, "Finalizing class: %s\n", SHOW(cls));
        set_final(cls);
        ++n_classes_finalized;
      }
    }
  }
  return n_classes_finalized;
}

const DexMethod* find_override(const DexMethod* method, const DexClass* cls) {
  std::vector<const DexType*> children;
  get_all_children(cls->get_type(), children);
  for (auto const& childtype : children) {
    auto const& child = type_class(childtype);
    assert(child);
    for (auto const& child_method : child->get_vmethods()) {
      if (signatures_match(method, child_method)) {
        return child_method;
      }
    }
  }
  return nullptr;
}

size_t mark_methods_final(const DexStoresVector& stores) {
  size_t n_methods_finalized = 0;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    for (auto const& cls : dex) {
      for (auto const& method : cls->get_vmethods()) {
        if (is_seed(method) || is_abstract(method) || is_final(method)) continue;
        if (!find_override(method, cls)) {
          TRACE(ACCESS, 2, "Finalizing method: %s\n", SHOW(method));
          set_final(method);
          ++n_methods_finalized;
        }
      }
    }
  }
  return n_methods_finalized;
}
}

void AccessMarkingPass::run_pass(
  DexStoresVector& stores,
  ConfigFiles& cfg,
  PassManager& pm
) {
  if (!cfg.using_seeds) {
    return;
  }
  auto n_classes_final = mark_classes_final(stores);
  auto n_methods_final = mark_methods_final(stores);
  pm.incr_metric("finalized_classes", n_classes_final);
  pm.incr_metric("finalized_methods", n_methods_final);
  TRACE(ACCESS, 1, "Finalized %lu classes\n", n_classes_final);
  TRACE(ACCESS, 1, "Finalized %lu methods\n", n_methods_final);
}

static AccessMarkingPass s_pass;
