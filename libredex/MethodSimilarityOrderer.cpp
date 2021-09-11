/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodSimilarityOrderer.h"

#include "DexInstruction.h"
#include "Show.h"
#include "Trace.h"

void MethodSimilarityOrderer::gather_code_hash_ids(
    const DexCode* code, std::unordered_set<CodeHashId>& code_hash_ids) {
  auto& instructions = code->get_instructions();

  // First, we partition the instructions into chunks, where each chunk ends
  // when an instruction can change control-flow.
  struct Chunk {
    size_t start;
    size_t end;
  };
  std::vector<Chunk> chunks;
  size_t start{0};
  for (size_t i = 0; i < instructions.size(); i++) {
    auto op = instructions.at(i)->opcode();
    if (dex_opcode::is_branch(op) || dex_opcode::is_return(op) ||
        op == DOPCODE_THROW) {
      chunks.push_back({start, i + 1});
      start = i;
    }
  }

  // For any instruction-sequence, we can compute a (stable) hash representing
  // it
  auto hash_sub_chunk = [&](const Chunk& sub_chunk) {
    always_assert(sub_chunk.start < sub_chunk.end);
    uint64_t code_hash{0};
    for (size_t i = sub_chunk.start; i < sub_chunk.end; i++) {
      auto insn = instructions.at(i);
      auto op = insn->opcode();
      code_hash = code_hash * 23 + op;
      if (insn->has_literal()) {
        code_hash = code_hash * 7 + insn->get_literal();
      }
      if (insn->has_range()) {
        code_hash =
            (code_hash * 11 + insn->range_base()) * 11 + insn->range_size();
      }
      if (insn->has_dest()) {
        code_hash = code_hash * 13 + insn->dest();
      }
      for (unsigned j = 0; j < insn->srcs_size(); j++) {
        code_hash = code_hash * 17 + insn->src(j);
      }
    }
    auto it = m_code_hash_ids.find(code_hash);
    if (it == m_code_hash_ids.end()) {
      it = m_code_hash_ids.emplace(code_hash, m_code_hash_ids.size()).first;
    }
    code_hash_ids.insert(it->second);
  };

  // We'll further partition chunks into smaller pieces, and then hash those
  // sub-chunks
  for (auto& chunk : chunks) {
    constexpr size_t chunk_size = 3;
    if (chunk.end - chunk.start < chunk_size) {
      hash_sub_chunk(chunk);
      continue;
    }
    for (size_t i = chunk.start; i <= chunk.end - chunk_size; i++) {
      hash_sub_chunk({i, i + chunk_size});
    }
  }
}

void MethodSimilarityOrderer::insert(DexMethod* method) {
  always_assert(m_method_indices.count(method) == 0);
  size_t index = m_methods.size();
  m_methods.emplace(index, method);
  m_method_indices.emplace(method, index);

  auto& code_hash_ids = m_method_code_hash_ids[method];

  if (type_class(method->get_class())->is_perf_sensitive()) {
    return;
  }

  auto* code = method->get_dex_code();
  if (code) {
    gather_code_hash_ids(code, code_hash_ids);
  }

  for (auto code_hash_id : code_hash_ids) {
    m_code_hash_id_methods[code_hash_id].insert(method);
  }
}

DexMethod* MethodSimilarityOrderer::get_next() {
  if (m_methods.empty()) {
    return nullptr;
  }
  boost::optional<size_t> best_candidate_index;

  // If the next method is part of a perf sensitive class,
  // then do not look for a candidate, just preserve the
  // original order.
  auto* next_method = m_methods.begin()->second;
  bool is_next_perf_sensitive = m_method_code_hash_ids[next_method].empty();

  if (!is_next_perf_sensitive && !m_last_code_hash_ids.empty()) {
    // Similarity score for each candidate method, based on the number of
    // shared, missing and additional hash ids when compared to the previously
    // chosen method.
    struct Score {
      size_t shared{0};
      size_t missing{0};
      size_t additional{0};
      int value() const { return 2 * shared - missing - 2 * additional; }
    };
    std::unordered_map<DexMethod*, Score> candidate_scores;
    // To compute the score, we add up how many matching code-hash-ids we
    // have...
    for (auto code_hash_id : m_last_code_hash_ids) {
      for (auto method : m_code_hash_id_methods.at(code_hash_id)) {
        candidate_scores[method].shared++;
      }
    }
    // minus penalty points for every non-matching code-hash-id
    for (auto& p : candidate_scores) {
      auto& other_code_hash_ids = m_method_code_hash_ids.at(p.first);

      size_t other_code_hash_ids_size = other_code_hash_ids.size();
      size_t last_code_hash_ids_size = m_last_code_hash_ids.size();

      p.second.additional = other_code_hash_ids_size - p.second.shared;
      p.second.missing = last_code_hash_ids_size - p.second.shared;
    }
    // Then we'll find the best matching candidate with a non-negative score
    // that is not perf sensitive.
    boost::optional<Score> best_candidate_score;
    for (auto& p : candidate_scores) {
      auto& score = p.second;
      if (score.value() < 0 || m_method_code_hash_ids[p.first].empty()) {
        continue;
      }
      if (!best_candidate_score ||
          score.value() > best_candidate_score->value() ||
          (score.value() == best_candidate_score->value() &&
           best_candidate_index &&
           m_method_indices.at(p.first) < *best_candidate_index)) {
        best_candidate_index = m_method_indices.at(p.first);
        best_candidate_score = p.second;
      }
    }
    if (best_candidate_score) {
      TRACE(
          OPUT, 3,
          "[method-similarity-orderer]   selected %s with %d = %zu - %zu - %zu",
          SHOW(m_methods.at(*best_candidate_index)),
          best_candidate_score->value(), best_candidate_score->shared,
          best_candidate_score->missing, best_candidate_score->additional);
    }
  }
  if (!best_candidate_index) {
    best_candidate_index = m_methods.begin()->first;
    TRACE(OPUT, 3, "[method-similarity-orderer] reverted to %s",
          SHOW(m_methods.at(*best_candidate_index)));
  }
  auto best_candidate_method = m_methods.at(*best_candidate_index);
  m_last_code_hash_ids =
      std::move(m_method_code_hash_ids.at(best_candidate_method));
  m_methods.erase(*best_candidate_index);
  m_method_code_hash_ids.erase(best_candidate_method);
  for (auto it = m_last_code_hash_ids.begin();
       it != m_last_code_hash_ids.end();) {
    auto code_hash_id = *it;
    auto& methods = m_code_hash_id_methods.at(code_hash_id);
    methods.erase(best_candidate_method);
    if (methods.empty()) {
      m_code_hash_id_methods.erase(code_hash_id);
      it = m_last_code_hash_ids.erase(it);
    } else {
      it++;
    }
  }
  return best_candidate_method;
}
