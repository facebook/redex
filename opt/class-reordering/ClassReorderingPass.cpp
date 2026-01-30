/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassReorderingPass.h"

#include "ConfigFiles.h"
#include "DeterministicContainers.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"

namespace {
size_t rearrange_dex(DexClasses& dex) {
  size_t num_inserted = 0;

  auto new_dex = DexClasses();
  new_dex.reserve(dex.size());
  UnorderedSet<DexType*> class_types_in_dex;
  UnorderedSet<DexType*> inserted;

  std::function<size_t(DexType*)> insert_class_and_its_hierachy =
      [&](DexType* cls_type) -> size_t {
    if (class_types_in_dex.find(cls_type) == class_types_in_dex.end()) {
      // Such class does not exist the dex originally; no need to insert
      return 0;
    }
    if (inserted.find(cls_type) != inserted.end()) {
      // already inserted, no need to continue
      return 0;
    }
    inserted.insert(cls_type);
    size_t insert_amount = 1;
    auto* cls = type_class(cls_type);
    for (auto* intf : *cls->get_interfaces()) {
      insert_amount += insert_class_and_its_hierachy(intf);
    }
    insert_amount += insert_class_and_its_hierachy(cls->get_super_class());
    new_dex.emplace_back(cls);
    return insert_amount;
  };

  for (auto* cls : dex) {
    class_types_in_dex.emplace(cls->get_type());
  }

  for (auto* cls : dex) {
    auto inserted_amount = insert_class_and_its_hierachy(cls->get_type());
    if (inserted_amount > 1) {
      num_inserted += (inserted_amount - 1);
    }
  }
  if (num_inserted > 0) {
    dex = std::move(new_dex);
  }
  return num_inserted;
}
} // end namespace

void ClassReorderingPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& /*conf*/,
                                   PassManager& mgr) {
  for (auto& store : stores) {
    Timer t("Writing optimized dexes");
    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      auto num_inserted = rearrange_dex(store.get_dexen()[i]);
      if (num_inserted > 0) {
        mgr.incr_metric(store.get_name() + std::to_string(i), num_inserted);
      }
    }
  }
}

static ClassReorderingPass s_pass;
