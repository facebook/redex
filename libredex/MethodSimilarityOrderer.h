/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <unordered_map>
#include <unordered_set>

#include <boost/dynamic_bitset.hpp>

#include "DexClass.h"
#include "Timer.h"

class DexInstruction;

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
 public:
  // Converted unique id (32 bit) for a sequence (chunk) of instructions
  // represented by stable_hash.
  using CodeHashId = uint32_t;

  using StableHash = uint64_t;

  // This is a synthetic 16 bit id, representing a method's index in the
  // original ordering. There should always be less than 65536 code items
  // to be ordered in a DEX.
  using MethodId = uint16_t;

  using ScoreValue = int32_t;

 private:
  // Mirrors the order in each the methods have been added to the orderer
  std::map<MethodId, DexMethod*> m_id_to_method;

  // The inverse of m_methods
  std::unordered_map<DexMethod*, MethodId> m_method_to_id;

  // Mapping from each method to a vector of hash ids
  std::unordered_map<MethodId, std::vector<CodeHashId>>
      m_method_id_to_code_hash_ids;

  // Vector to contain similarity score among all Methods for the given
  // buffer indexed Method. WorkQueue accesses this vector concurrently.
  std::vector<
      std::map<ScoreValue, boost::dynamic_bitset<>, std::greater<ScoreValue>>>
      m_score_map;

  // Last Method Id that is ordered.
  boost::optional<MethodId> m_last_method_id;

  // Mapping from stable hash (64 bit) to code hash id (32 bit). This saves
  // space.
  std::unordered_map<StableHash, CodeHashId> m_stable_hash_to_code_hash_id;

  void insert(DexMethod* method);

  void remove_method(DexMethod* meth);

  // Gather the hash ids of all instruction sequences
  // (of a certain size) inside code
  void gather_code_hash_ids(const std::vector<DexInstruction*>& instructions,
                            std::vector<CodeHashId>& code_hash_ids);

  boost::optional<MethodId> get_next();

  void compute_score();

 public:
  void order(std::vector<DexMethod*>& methods);
};
