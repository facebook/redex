/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <unordered_set>

#include "DexClass.h"
#include "Model.h"
#include "Trace.h"

class DexStore;
class PassManager;
class TypeTags;

using DexStoresVector = std::vector<DexStore>;

namespace class_merging {

struct MergerType;
class ModelMethodMerger;
using MergerFields = std::unordered_map<const DexType*, std::vector<DexField*>>;

class ModelMerger {

 public:
  virtual ~ModelMerger(){};

  std::vector<DexClass*> merge_model(Scope& scope,
                                     DexStoresVector& stores,
                                     const ConfigFiles& conf,
                                     Model& model);

  void update_redex_stats(const std::string& prefix, PassManager& mgr) const;

  void increase_ctor_dedupped_stats(int64_t value) {
    m_stats.m_num_ctor_dedupped += value;
  }

 protected:
  virtual void post_process(
      Model& model,
      TypeTags& type_tags,
      std::unordered_map<const DexType*, const DexMethod*>&
          mergeable_to_merger_ctor) {
    TRACE(CLMG, 5, "[ClassMerging] default post process");
  }

 private:
  ModelStats m_stats;
  static const std::vector<DexField*> empty_fields;
  MergerFields m_merger_fields;

  void update_merger_fields(const MergerType& merger);
  void update_stats(const std::string& name,
                    const std::vector<const MergerType*>& mergers,
                    ModelMethodMerger& mm);
};

} // namespace class_merging
