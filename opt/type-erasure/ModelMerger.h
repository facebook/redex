/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <unordered_set>

#include "DexClass.h"
#include "PassManager.h"

class Model;
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

  uint32_t get_num_classes_merged() const { return m_num_classes_merged; }
  uint32_t get_num_ctor_dedupped() const { return m_num_ctor_dedupped; }
  uint32_t get_num_static_non_virt_dedupped() const {
    return m_num_static_non_virt_dedupped;
  }
  uint32_t get_num_vmethods_dedupped() const { return m_num_vmethods_dedupped; }
  uint32_t get_num_const_lifted_methods() const {
    return m_num_const_lifted_methods;
  }

  void update_redex_stats(const std::string& prefix, PassManager& mgr) const;

  static std::string s_mapping_file;

 protected:
  virtual void post_process(
      Model& model,
      TypeTags& type_tags,
      std::unordered_map<const DexType*, const DexMethod*>&
          mergeable_to_merger_ctor) {
    TRACE(TERA, 5, "[TERA] default post process\n");
  }

 private:
  uint32_t m_num_classes_merged = 0;
  uint32_t m_num_ctor_dedupped = 0;
  uint32_t m_num_static_non_virt_dedupped = 0;
  uint32_t m_num_vmethods_dedupped = 0;
  uint32_t m_num_const_lifted_methods = 0;
  uint32_t m_num_generated_classes = 0;
  static const std::vector<DexField*> empty_fields;
  MergerFields m_merger_fields;

  void update_merger_fields(const MergerType& merger);
  void update_stats(const std::string name,
                    const std::vector<const MergerType*>& mergers,
                    ModelMethodMerger& mm);
};
