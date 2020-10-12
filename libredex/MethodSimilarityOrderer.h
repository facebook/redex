/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include "DexClass.h"

class MethodSimilarityOrderer {
  using CodeHashId = size_t;
  std::unordered_set<CodeHashId> m_last_code_hash_ids;
  std::map<size_t, DexMethod*> m_methods;
  std::unordered_map<DexMethod*, size_t> m_method_indices;
  std::unordered_map<DexMethod*, std::unordered_set<CodeHashId>>
      m_method_code_hash_ids;
  std::unordered_map<CodeHashId, std::unordered_set<DexMethod*>>
      m_code_hash_id_methods;
  std::unordered_map<uint64_t, CodeHashId> m_code_hash_ids;

  void insert(DexMethod* method);
  void gather_code_hash_ids(const DexCode* code,
                            std::unordered_set<CodeHashId>& code_hash_ids);

 public:
  explicit MethodSimilarityOrderer(const std::vector<DexMethod*>& methods) {
    for (auto method : methods) {
      insert(method);
    }
  }
  DexMethod* get_next();
  static void order(std::vector<DexMethod*>& methods) {
    MethodSimilarityOrderer mso(methods);
    methods.clear();
    DexMethod* method;
    while ((method = mso.get_next()) != nullptr) {
      methods.push_back(method);
    }
  }
};
