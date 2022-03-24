/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodSimilarityOrderer.h"

#include "DexInstruction.h"
#include "Show.h"
#include "Trace.h"
#include "WorkQueue.h"

namespace {
// Similarity score for each candidate method, based on the number of
// shared, missing and additional hash ids when compared to the previously
// chosen method.
struct Score {
  uint32_t shared{0};
  uint32_t missing{0};
  uint32_t additional{0};
  int32_t value() const { return 2 * shared - missing - 2 * additional; }
};

struct Chunk {
  uint32_t start;
  uint32_t end;
};

template <typename T>
struct CountingFakeOutputIterator {
  uint32_t& counter;
  T fake;
  explicit CountingFakeOutputIterator(uint32_t& c) : counter(c) {}
  CountingFakeOutputIterator& operator++() {
    counter++;
    return *this;
  }
  T& operator*() { return fake; }
};
}; // namespace

void MethodSimilarityOrderer::gather_code_hash_ids(
    const std::vector<DexInstruction*>& instructions,
    std::vector<CodeHashId>& code_hash_ids) {

  std::vector<Chunk> chunks;
  uint32_t start{0};

  // First, we partition the instructions into chunks, where each chunk ends
  // when an instruction can change control-flow.
  for (uint32_t i = 0; i < (uint32_t)instructions.size(); i++) {
    auto op = instructions.at(i)->opcode();
    if (dex_opcode::is_branch(op) || dex_opcode::is_return(op) ||
        op == DOPCODE_THROW) {
      chunks.push_back({start, i + 1});
      start = i;
    }
  }

  std::unordered_set<StableHash> stable_hashes;

  // For any instruction-sequence, we can compute a (stable) hash representing
  // it
  auto hash_sub_chunk = [&](const Chunk& sub_chunk) {
    always_assert(sub_chunk.start < sub_chunk.end);
    StableHash code_hash{0};
    for (uint32_t i = sub_chunk.start; i < sub_chunk.end; i++) {
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
    stable_hashes.insert(code_hash);
  };

  // We'll further partition chunks into smaller pieces, and then hash those
  // sub-chunks
  for (auto& chunk : chunks) {
    constexpr uint32_t chunk_size = 1;
    if (chunk.end - chunk.start < chunk_size) {
      hash_sub_chunk(chunk);
      continue;
    }
    for (uint32_t i = chunk.start; i <= chunk.end - chunk_size; i++) {
      hash_sub_chunk({i, i + chunk_size});
    }
  }

  // Initialize the vector for code hash.
  // Publish code hash ids from stable hashes.
  code_hash_ids.reserve(stable_hashes.size());
  for (StableHash stable_hash : stable_hashes) {
    if (m_stable_hash_to_code_hash_id.count(stable_hash) == 0) {
      // Assign a unique code hash id if it appears for the first time.
      auto code_hash_id = m_stable_hash_to_code_hash_id.size();
      m_stable_hash_to_code_hash_id[stable_hash] = code_hash_id;
    }
    code_hash_ids.push_back(m_stable_hash_to_code_hash_id[stable_hash]);
  }
  // Sort the code hash Ids to check the overlaps in the linear time.
  std::sort(code_hash_ids.begin(), code_hash_ids.end());
}

// For the given code hash ids (i) and code hash ids (j), get score for j
// against i.
static inline Score get_score(
    const std::vector<MethodSimilarityOrderer::CodeHashId>& code_hash_ids_i,
    const std::vector<MethodSimilarityOrderer::CodeHashId>& code_hash_ids_j) {
  uint32_t i_size = code_hash_ids_i.size();
  uint32_t j_size = code_hash_ids_j.size();

  Score score;
  std::set_intersection(
      code_hash_ids_i.begin(), code_hash_ids_i.end(), code_hash_ids_j.begin(),
      code_hash_ids_j.end(),
      CountingFakeOutputIterator<MethodSimilarityOrderer::CodeHashId>(
          score.shared));

  score.missing = i_size - score.shared;
  score.additional = j_size - score.shared;

  return score;
}

void MethodSimilarityOrderer::compute_score() {
  m_score_map.clear();
  m_score_map.resize(m_id_to_method.size());

  std::vector<MethodId> method_ids;
  for (auto& p : m_id_to_method) {
    auto method_id = p.first;
    m_method_id_to_buffer_id[method_id] = method_ids.size();
    method_ids.push_back(method_id);
  }

  std::vector<uint32_t> indices(method_ids.size());
  std::iota(indices.begin(), indices.end(), 0);
  workqueue_run<uint32_t>(
      [&](uint32_t i) {
        MethodId method_i = method_ids[i];
        const auto& code_hash_ids_i = m_method_id_to_code_hash_ids[method_i];
        std::unordered_map<ScoreValue, boost::dynamic_bitset<>> score_map;

        for (uint32_t j = 0; j < (uint32_t)method_ids.size(); j++) {
          if (i == j) {
            continue;
          }

          MethodId method_j = method_ids[j];
          const auto& code_hash_ids_j = m_method_id_to_code_hash_ids[method_j];
          auto score = get_score(code_hash_ids_i, code_hash_ids_j);
          if (score.value() >= 0) {
            auto& method_id_bitset = score_map[score.value()];
            if (method_id_bitset.size() <= method_j) {
              method_id_bitset.resize(method_j + 1);
            }
            method_id_bitset.set(static_cast<size_t>(method_j));
          }
        }

        // Mapping from score value (key) to Method Ids. The key is in a
        // decreasing score order. Becuase it iterates Method Id in order,
        // the vector is already sorted by Method index (source order).
        std::map<ScoreValue, boost::dynamic_bitset<>, std::greater<ScoreValue>>
            map(score_map.begin(), score_map.end());
        m_score_map[i] = std::move(map);
      },
      indices);
}

void MethodSimilarityOrderer::insert(DexMethod* method) {
  always_assert(m_method_to_id.count(method) == 0);
  uint32_t index = m_id_to_method.size();
  m_id_to_method.emplace(index, method);
  m_method_to_id.emplace(method, index);

  auto& code_hash_ids = m_method_id_to_code_hash_ids[index];

  if (type_class(method->get_class())->is_perf_sensitive()) {
    return;
  }

  auto* code = method->get_dex_code();
  if (code) {
    gather_code_hash_ids(code->get_instructions(), code_hash_ids);
  }
}

boost::optional<MethodSimilarityOrderer::MethodId>
MethodSimilarityOrderer::get_next() {
  // Clear best candidates.
  if (m_id_to_method.empty()) {
    return {};
  }

  // If the next method is part of a perf sensitive class,
  // then do not look for a candidate, just preserve the
  // original order.
  auto method_id = m_id_to_method.begin()->first;
  bool is_next_perf_sensitive = m_method_id_to_code_hash_ids[method_id].empty();

  if (!is_next_perf_sensitive && m_last_method_id != boost::none) {
    // Iterate m_score_map from the highest score..
    for (const auto& [score, method_id_bitset] :
         m_score_map[m_method_id_to_buffer_id[*m_last_method_id]]) {
      for (auto meth_id = method_id_bitset.find_first();
           meth_id != boost::dynamic_bitset<>::npos;
           meth_id = method_id_bitset.find_next(meth_id)) {
        // The first match is the one with the highest score
        // at the smallest index in the source order.
        if (m_id_to_method.count(static_cast<MethodId>(meth_id))) {
          TRACE(OPUT, 3,
                "[method-similarity-orderer] selected %s with score %d",
                SHOW(m_id_to_method[static_cast<MethodId>(meth_id)]), score);
          return meth_id;
        }
      }
    }
  }
  boost::optional<MethodId> best_method_id = m_id_to_method.begin()->first;
  TRACE(OPUT,
        3,
        "[method-similarity-orderer] reverted to %s",
        SHOW(*best_method_id));
  return best_method_id;
}

void MethodSimilarityOrderer::remove_method(DexMethod* meth) {
  if (!m_method_to_id.count(meth)) return;

  auto method_id = m_method_to_id[meth];
  m_id_to_method.erase(method_id);
  m_method_to_id.erase(meth);
}

void MethodSimilarityOrderer::order(std::vector<DexMethod*>& methods) {
  Timer t("Reordering methods by similarity");
  for (auto* method : methods) {
    insert(method);
  }

  methods.clear();

  // Compute scores among methods in parallel.
  compute_score();

  m_last_method_id = boost::none;
  for (boost::optional<MethodId> best_method_id = get_next();
       best_method_id != boost::none;
       best_method_id = get_next()) {
    m_last_method_id = best_method_id;
    auto meth = m_id_to_method[*m_last_method_id];
    methods.push_back(meth);
    remove_method(meth);
  }
}
