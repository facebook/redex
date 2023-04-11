/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BalancedPartitioning.h"

#include <algorithm>
#include <cassert>
#include <optional>

#include "Debug.h"
#include "WorkQueue.h"

BalancedPartitioning::BalancedPartitioning(std::vector<Document*>& documents)
    : documents(documents) {

  // Pre-computing log2 values
  LOG2_CACHE[0] = 0.0;
  for (uint32_t i = 1; i < LOG_CACHE_SIZE; i++) {
    LOG2_CACHE[i] = std::log2(i);
  }
}

void BalancedPartitioning::run() const {
  for (uint32_t i = 0; i < documents.size(); i++) {
    documents[i]->hash = i;
  }

  /// Run a recursive bisection of a given list of documents where
  ///  - 'rec_depth' is the current depth of recursion
  ///  - 'root_bucket' is the initial bucket of the dataVertices
  ///  - the assigned buckets are the range [offset, offset + num_documents)
  struct WorkItem {
    const std::vector<Document*>::iterator document_begin;
    const std::vector<Document*>::iterator document_end;
    uint32_t rec_depth;
    uint32_t root_bucket;
    uint32_t offset;
  };

  auto wq = workqueue_foreach<WorkItem>(
      [&](sparta::SpartaWorkerState<WorkItem>* worker_state,
          const WorkItem& work_item) {
        uint32_t num_documents =
            std::distance(work_item.document_begin, work_item.document_end);
        if (num_documents == 0) return;

        // Reached the lowest level of the recursion tree
        if (work_item.rec_depth >= SPLIT_DEPTH || num_documents <= 1) {
          order(work_item.document_begin, work_item.document_end,
                work_item.offset);
          return;
        }

        std::mt19937 rng(work_item.root_bucket);

        uint32_t left_bucket = 2 * work_item.root_bucket;
        uint32_t right_bucket = 2 * work_item.root_bucket + 1;

        // Initialize 2 buckets
        split(work_item.document_begin, work_item.document_end, left_bucket);

        // Do iterations to improve the objective
        run_iterations(work_item.document_begin, work_item.document_end,
                       left_bucket, right_bucket, rng);

        // Split documents wrt the resulting buckets
        auto document_mid =
            std::partition(work_item.document_begin, work_item.document_end,
                           [left_bucket](const Document* doc) {
                             return doc->bucket == left_bucket;
                           });

        uint32_t mid_offset =
            work_item.offset +
            std::distance(work_item.document_begin, document_mid);

        // Two recursive tasks
        worker_state->push_task(
            (WorkItem){work_item.document_begin, document_mid,
                       work_item.rec_depth + 1, left_bucket, work_item.offset});
        worker_state->push_task((WorkItem){document_mid, work_item.document_end,
                                           work_item.rec_depth + 1,
                                           right_bucket, mid_offset});
      },
      redex_parallel::default_num_threads(), /*push_tasks_while_running=*/true);
  wq.add_item((WorkItem){begin(documents), end(documents), 0, 1, 0});
  wq.run_all();
}

void BalancedPartitioning::run_iterations(
    const std::vector<Document*>::iterator& document_begin,
    const std::vector<Document*>::iterator& document_end,
    uint32_t left_bucket,
    uint32_t right_bucket,
    std::mt19937& rng) const {
  // Initialize document adjacencies: renumber kmers and drop obsolete ones
  uint32_t max_kmer = update_documents(document_begin, document_end);

  // Initialize signatures
  SignaturesType signatures(max_kmer + 1);
  initialize_signatures(signatures, document_begin, document_end, left_bucket);

  // Run iterations
  for (uint32_t iter = 0; iter < ITERATIONS_PER_SPLIT; iter++) {
    uint32_t num_moved_documents =
        run_iteration(document_begin, document_end, left_bucket, right_bucket,
                      signatures, rng);
    if (num_moved_documents == 0) break;
  }
}

uint32_t BalancedPartitioning::update_documents(
    const std::vector<Document*>::iterator& document_begin,
    const std::vector<Document*>::iterator& document_end) const {
  uint32_t num_documents = std::distance(document_begin, document_end);

  // Get the maximum kmer adjacent to the given set of documents
  uint32_t max_kmer = 0;
  for (auto it = document_begin; it != document_end; it++) {
    Document* doc = *it;
    const auto& vec = doc->adjacent_kmers();
    if (!vec.empty()) {
      max_kmer = std::max(max_kmer, *std::max_element(vec.begin(), vec.end()));
    }
  }

  // Count the (local) degree of each kmer and compute their new indices
  std::vector<std::optional<uint32_t>> kmer_index(max_kmer + 1);
  std::vector<uint32_t> kmer_degree(max_kmer + 1);
  uint32_t num_kmers = 0;
  for (auto it = document_begin; it != document_end; it++) {
    Document* doc = *it;
    for (uint32_t kmer : doc->adjacent_kmers()) {
      if (!kmer_index[kmer]) {
        kmer_index[kmer] = num_kmers;
        num_kmers++;
      }
      kmer_degree[*kmer_index[kmer]]++;
    }
  }

  // Update document adjacency lists
  max_kmer = 0;
  for (auto it = document_begin; it != document_end; it++) {
    Document* doc = *it;
    std::vector<uint32_t> new_kmers;
    new_kmers.reserve(doc->size());
    for (uint32_t kmer : doc->adjacent_kmers()) {
      uint32_t kmer_idx = *kmer_index[kmer];
      always_assert_log(1 <= kmer_degree[kmer_idx] &&
                            kmer_degree[kmer_idx] <= num_documents,
                        "Incorrect degree of a k-mer");
      // Ignore useless kmers that do not affect the optimization
      if (1 < kmer_degree[kmer_idx] && kmer_degree[kmer_idx] < num_documents) {
        new_kmers.push_back(kmer_idx);
        max_kmer = std::max(kmer_idx, max_kmer);
      }
    }
    doc->assign(new_kmers);
  }
  return max_kmer;
}

void BalancedPartitioning::initialize_signatures(
    SignaturesType& signatures,
    const std::vector<Document*>::iterator& document_begin,
    const std::vector<Document*>::iterator& document_end,
    uint32_t left_bucket) const {
  for (auto it = document_begin; it != document_end; it++) {
    Document* doc = *it;
    // To avoid an unpredictable branch in the loop, write two loops separately
    if (doc->bucket == left_bucket) {
      for (uint32_t kmer : doc->adjacent_kmers()) {
        signatures.at(kmer).left_count++;
      }
    } else {
      for (uint32_t kmer : doc->adjacent_kmers()) {
        signatures.at(kmer).right_count++;
      }
    }
  }
}

uint32_t BalancedPartitioning::run_iteration(
    const std::vector<Document*>::iterator& document_begin,
    const std::vector<Document*>::iterator& document_end,
    uint32_t left_bucket,
    uint32_t right_bucket,
    SignaturesType& signatures,
    std::mt19937& rng) const {
  // Initialize signature caches, if needed
  for (KmerSignature& signature : signatures) {
    if (signature.cache_is_invalid &&
        (signature.left_count > 0 || signature.right_count > 0)) {
      prepare_signature(signature);
      signature.cache_is_invalid = false;
    }
  }

  // Compute move gains
  uint32_t num_documents =
      uint32_t(std::distance(document_begin, document_end));
  using GainPair = std::pair<double, uint32_t>;
  std::vector<GainPair> gains(num_documents);
  for (auto it = document_begin; it != document_end; it++) {
    Document* doc = *it;
    uint32_t index = it - document_begin;
    bool from_left_to_right = (doc->bucket == left_bucket);
    double gain = move_gain(doc, from_left_to_right, signatures);
    gains[index] = std::make_pair(gain, index);
  }

  // Collect left and right gains
  auto left_gains = gains.begin();
  auto left_end =
      std::partition(gains.begin(), gains.end(), [&](const GainPair& GP) {
        return document_begin[GP.second]->bucket == left_bucket;
      });

  auto right_gains = left_end;
  auto right_end = gains.end();

  // Sort gains
  std::sort(left_gains, left_end, std::greater<GainPair>());
  std::sort(right_gains, right_end, std::greater<GainPair>());

  // Exchange: change buckets and update queryVertex signatures
  uint32_t num_moved_data_vertices = 0;
  uint32_t min_size = std::min(std::distance(left_gains, left_end),
                               std::distance(right_gains, right_end));
  for (uint32_t I = 0; I < min_size; I++) {
    if (left_gains[I].first + right_gains[I].first <= 0.0) break;
    // Try to swap the two documents
    num_moved_data_vertices +=
        move_data_vertex(document_begin[left_gains[I].second], left_bucket,
                         right_bucket, signatures, rng);
    num_moved_data_vertices +=
        move_data_vertex(document_begin[right_gains[I].second], left_bucket,
                         right_bucket, signatures, rng);
  }
  return num_moved_data_vertices;
}

bool BalancedPartitioning::move_data_vertex(Document* doc,
                                            uint32_t left_bucket,
                                            uint32_t right_bucket,
                                            SignaturesType& signatures,
                                            std::mt19937& rng) const {
  // Sometimes we skip the move. This helps to escape local optima
  if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) <=
      SKIP_PROBABILITY) {
    return false;
  }

  // Update the current bucket and all signatures
  if (doc->bucket == left_bucket) {
    doc->bucket = right_bucket;
    for (uint32_t kmer : doc->adjacent_kmers()) {
      KmerSignature& signature = signatures.at(kmer);
      signature.cache_is_invalid = true;
      signature.left_count--;
      signature.right_count++;
    }
  } else {
    doc->bucket = left_bucket;
    for (uint32_t kmer : doc->adjacent_kmers()) {
      KmerSignature& signature = signatures.at(kmer);
      signature.cache_is_invalid = true;
      signature.left_count++;
      signature.right_count--;
    }
  }

  return true;
}

void BalancedPartitioning::order(
    const std::vector<Document*>::iterator& document_begin,
    const std::vector<Document*>::iterator& document_end,
    uint32_t start_bucket) const {
  // Sort documents by min-hash
  std::sort(document_begin, document_end,
            [](const Document* doc_l, const Document* doc_r) {
              return doc_l->hash < doc_r->hash;
            });

  // Assign buckets
  for (auto it = document_begin; it != document_end; it++) {
    (*it)->bucket = start_bucket++;
  }
}

void BalancedPartitioning::split(
    const std::vector<Document*>::iterator& document_begin,
    const std::vector<Document*>::iterator& document_end,
    uint32_t start_bucket) const {
  uint32_t num_documents = std::distance(document_begin, document_end);
  always_assert_log(num_documents > 0, "Incorrect number of documents %u",
                    num_documents);

  uint32_t docs_per_bucket = (num_documents + 1) / 2;
  always_assert_log(docs_per_bucket > 0,
                    "Incorrect number of docs per bucket %u", docs_per_bucket);

  // We just need partitioning, so can use O(n) nth_element.
  std::nth_element(document_begin, document_begin + docs_per_bucket,
                   document_end,
                   [](const Document* doc_l, const Document* doc_r) {
                     return doc_l->hash < doc_r->hash;
                   });

  // Assign buckets
  auto it = document_begin;
  for (uint32_t I = 0; I < 2; I++) {
    for (uint32_t J = 0; J < docs_per_bucket && it != document_end; J++) {
      (*it++)->bucket = start_bucket + I;
    }
  }
}

double BalancedPartitioning::move_gain(const Document* doc,
                                       bool from_left_to_right,
                                       const SignaturesType& signatures) const {
  double gain = 0;
  // To avoid an unpredictable branch in the loop, we write two loops separately
  if (from_left_to_right) {
    for (uint32_t kmer : doc->adjacent_kmers()) {
      gain += signatures.at(kmer).cached_cost_lr;
    }
  } else {
    for (uint32_t kmer : doc->adjacent_kmers()) {
      gain += signatures.at(kmer).cached_cost_rl;
    }
  }
  return gain;
}

void BalancedPartitioning::prepare_signature(KmerSignature& signature) const {
  uint32_t l = signature.left_count;
  uint32_t r = signature.right_count;
  always_assert_log(l > 0 || r > 0, "Incorrect signature (l: %u, r: %u)", l, r);
  double cost = log_cost(l, r);
  if (l > 0) {
    signature.cached_cost_lr = cost - log_cost(l - 1, r + 1);
  }
  if (r > 0) {
    signature.cached_cost_rl = cost - log_cost(l + 1, r - 1);
  }
}

double BalancedPartitioning::log_cost(uint32_t x, uint32_t y) const {
  // A faster way of computing std::log2(x + 1) and std::log2(y + 1), using
  // pre-computed values
  double log_x1 = x + 1 < LOG_CACHE_SIZE ? LOG2_CACHE[x + 1] : std::log2(x + 1);
  double log_y1 = y + 1 < LOG_CACHE_SIZE ? LOG2_CACHE[y + 1] : std::log2(y + 1);
  return -(x * log_x1 + y * log_y1);
}
