/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include "DexClass.h"

/**
 * Method ordering based on code similarity. Methods are selected one after
 * another and, whenever a method has previously been picked, the logic checks
 * whether there is a remaining method that shares many similar instruction
 * (sub)sequences and has few differing instruction (sub)sequences. When
 * there is no next similar method that stands out, the orderer reverts back to
 * the original method order.
 *
 * In other words, we only change the original method order by lumping together
 * highly similar methods. For example, methods with a small body like "return
 * true;" would all get co-located right after the first such method, resulting
 * in better compression.
 */
class MethodSimilarityOrderer {
  // Hash id for a sequence (chunk) of instructions
  using CodeHashId = size_t;

  // Set of hash ids belonging to the previously chosen method code
  std::unordered_set<CodeHashId> m_last_code_hash_ids;

  // Mirrors the order in each the methods have been added to the orderer
  std::map<size_t, DexMethod*> m_methods;

  // The inverse of m_methods
  std::unordered_map<DexMethod*, size_t> m_method_indices;

  // Mapping from each method to its set of hash ids
  std::unordered_map<DexMethod*, std::unordered_set<CodeHashId>>
      m_method_code_hash_ids;

  // Mapping from hash ids to the sets of all methods containing a sequence
  // of instructions with that hash id
  std::unordered_map<CodeHashId, std::unordered_set<DexMethod*>>
      m_code_hash_id_methods;

  // Mapping from hashed code sequences to their respective hash ids
  std::unordered_map<uint64_t, CodeHashId> m_code_hash_ids;

  void insert(DexMethod* method);

  // Gather the hash ids of all instruction sequences
  // (of a certain size) inside code
  void gather_code_hash_ids(const DexCode* code,
                            std::unordered_set<CodeHashId>& code_hash_ids);

 public:
  explicit MethodSimilarityOrderer(const std::vector<DexMethod*>& methods) {
    for (auto* method : methods) {
      insert(method);
    }
  }

  DexMethod* get_next();

  static void order(std::vector<DexMethod*>& methods) {
    MethodSimilarityOrderer mso(methods);
    methods.clear();

    DexMethod* next_method;
    while ((next_method = mso.get_next()) != nullptr) {
      methods.push_back(next_method);
    }
  }
};
