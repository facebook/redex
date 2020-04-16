/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <unordered_set>

#include "DexClass.h"
#include "Model.h"
#include "PassManager.h"

struct MergerType;
class ModelMethodMerger;
class DexStore;
class TypeTags;
using DexStoresVector = std::vector<DexStore>;
using MergerFields = std::unordered_map<const DexType*, std::vector<DexField*>>;

class ModelMerger {

 public:
  virtual ~ModelMerger(){};

  std::vector<DexClass*> merge_model(
      Scope& scope,
      DexStoresVector& stores,
      Model& model,
      boost::optional<size_t> max_num_dispatch_target = boost::none);

  void update_redex_stats(const std::string& prefix, PassManager& mgr) const;

  static std::string s_mapping_file;

 protected:
  virtual void post_process(
      Model& model,
      TypeTags& type_tags,
      std::unordered_map<const DexType*, const DexMethod*>&
          mergeable_to_merger_ctor) {
    TRACE(TERA, 5, "[TERA] default post process");
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
