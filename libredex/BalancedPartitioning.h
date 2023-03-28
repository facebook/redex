/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <random>
#include <string>
#include <vector>

class Document;
class KmerSignature;

/**
 * Recursive balanced graph partitioning algorithm.
 *
 * The algorithm is used to find an ordering of Documents while optimizing
 * a specified objective. The algorithm uses recursive bisection; it starts
 * with a collection of unordered documents and tries to split them into
 * two sets (buckets) of equal cardinality. Each bisection step is comprised of
 * iterations that greedily swap the documents between the two buckets while
 * there is an improvement of the objective. Once the process converges, the
 * problem is divided into two sub-problems of half the size, which are
 * recursively applied for the two buckets. The final ordering of the documents
 * is obtained by concatenating the two (recursively computed) orderings.
 *
 * In order to speed up the computation, we limit the depth of the recursive
 * tree by a specified constant (SplitDepth) and apply at most a constant
 * number of greedy iterations per split (IterationsPerSplit). The worst-case
 * time complexity of the implementation is bounded by O(M*log^2 N), where
 * N is the number of documents and M is the number of document-kmer edges;
 * (assuming that any collection of D documents contains O(D) k-mers). Notice
 * that the two different recursive sub-problems are independent and thus can
 * be efficiently processed in parallel.
 */
class BalancedPartitioning {
  using SignaturesType = std::vector<KmerSignature>;

 private:
  BalancedPartitioning(const BalancedPartitioning&) = delete;
  BalancedPartitioning& operator=(const BalancedPartitioning&) = delete;

 public:
  explicit BalancedPartitioning(std::vector<Document*>& documents);

  /// Run recursive graph partitioning that optimizes a given objective.
  void run() const;

 private:
  /// Run bisection iterations.
  /// Returns true iff a progress has been made.
  void run_iterations(const std::vector<Document*>::iterator& document_begin,
                      const std::vector<Document*>::iterator& document_end,
                      uint32_t left_bucket,
                      uint32_t right_bucket,
                      std::mt19937& rng) const;

  /// Run a bisection iteration to improve the optimization goal.
  /// Returns the total number of moved documents.
  uint32_t run_iteration(const std::vector<Document*>::iterator& document_begin,
                         const std::vector<Document*>::iterator& document_end,
                         uint32_t left_bucket,
                         uint32_t right_bucket,
                         SignaturesType& Signatures,
                         std::mt19937& rng) const;

  /// Try to move a document from one bucket to another.
  /// Return true iff the document is moved.
  bool move_data_vertex(Document* document,
                        uint32_t left_bucket,
                        uint32_t right_bucket,
                        SignaturesType& Signatures,
                        std::mt19937& rng) const;

  /// Update document adjacency lists.
  /// The method returns the maximum kmer index adjacent to the given documents.
  uint32_t update_documents(
      const std::vector<Document*>::iterator& document_begin,
      const std::vector<Document*>::iterator& document_end) const;

  /// Initialize k-mer signatures.
  void initialize_signatures(
      SignaturesType& Signatures,
      const std::vector<Document*>::iterator& document_begin,
      const std::vector<Document*>::iterator& document_end,
      uint32_t left_bucket) const;

  /// Split all the documents into 2 buckets, start_bucket and start_bucket + 1.
  /// The method is used for an initial assignment before a bisection step.
  void split(const std::vector<Document*>::iterator& document_begin,
             const std::vector<Document*>::iterator& document_end,
             uint32_t start_bucket) const;

  /// Order the list of documents by assigning buckets in the range
  /// [start_bucket + document_begin, start_bucket + document_end).
  /// The method is used for assigning buckets when the number of
  /// documents is small (to truncate the bisection tree).
  void order(const std::vector<Document*>::iterator& document_begin,
             const std::vector<Document*>::iterator& document_end,
             uint32_t start_bucket) const;

  /// Initialize k-mer signature before a bisection iteration.
  void prepare_signature(KmerSignature& signature) const;

  /// An average optimization goal for a given k-mer signature:
  /// - to represent an integer k, one needs log_2(k) bits;
  /// - to represent n integers in the range [0..U) (using the diff encoding),
  ///   one needs log_2(U/n) per number, since an average diff is U/n.
  /// Hence, n integers in the range [0..U) require (2 + log(U/n))*n bits, where
  /// two additional bits is a constant overhead.
  ///
  /// Compute the move gain for uniform log-gap cost:
  /// cost = x * log(U / (x+1)) + y * log(U / (y+1)) =
  ///      = x * log(U) + y * log(U) - (x * log(x+1) + y * log(y+1)) =
  ///      = U * log(U) - (x * log(x+1) + y * log(y+1))
  /// The first term is constant; the second is 'log_cost'.
  double move_gain(const Document* doc,
                   bool from_left_to_right,
                   const SignaturesType& signatures) const;

  /// The cost of the uniform log-gap cost, assuming a k-mer has X
  /// documents in the left bucket and Y documents in the right one.
  double log_cost(uint32_t x, uint32_t y) const;

  /// Input documents that shall be reordered by the algorithm.
  std::vector<Document*>& documents;

  /// Precomputed values of log2(x). Table size is small enough to fit in cache.
  static constexpr uint32_t LOG_CACHE_SIZE = 16384;
  double LOG2_CACHE[LOG_CACHE_SIZE];

  /// Algorithm parameters; default values are tuned on real-world binaries.
  ///
  /// The depth of the recursive bisection.
  uint32_t SPLIT_DEPTH = 18;
  /// The maximum number of bp iterations per split.
  uint32_t ITERATIONS_PER_SPLIT = 40;
  /// The probability for a vertex to skip a move from its current bucket to
  /// another bucket; it often helps to escape from a local optima.
  static constexpr double SKIP_PROBABILITY = 0.1;
};

/**
 * A document with edges to k-mers. After merging duplicates, the object
 * may represent a group of duplicate (or very similar) documents whose ids
 * are stored in the corresponding field.
 */
class Document {
 public:
  Document(const Document&) = delete;
  Document(Document&&) = default;
  Document& operator=(const Document&) = delete;
  Document& operator=(Document&&) = default;

  explicit Document() {}

  void init(uint32_t doc_id) { ids.push_back(doc_id); }

  void add(uint32_t adjacent_kmer) { edges.push_back(adjacent_kmer); }

  void assign(const std::vector<uint32_t>& adjacent_kmers) {
    edges.assign(adjacent_kmers.begin(), adjacent_kmers.end());
  }

  const std::vector<uint32_t>& adjacent_kmers() const { return edges; }

  void shrink_to_fit() { edges.shrink_to_fit(); }

  size_t size() const { return edges.size(); }

  /// Document bucket assigned by balanced partitioning.
  uint32_t bucket = uint32_t(-1);

  /// Hash code of the document based on its content.
  uint64_t hash = 0;

 private:
  /// Document ids of all (duplicate) documents corresponding to the instance.
  std::vector<uint32_t> ids;
  /// Adjacent k-mers of the document.
  std::vector<uint32_t> edges;
};

/**
 * Signature of a Kmer utilized in a bisection step, that is, the number of
 * incident documents in the two buckets.
 */
class KmerSignature {
 public:
  KmerSignature(const KmerSignature&) = delete;
  KmerSignature(KmerSignature&&) = default;
  KmerSignature& operator=(const KmerSignature&) = delete;
  KmerSignature& operator=(KmerSignature&&) = default;

  explicit KmerSignature(uint32_t left_count = 0, uint32_t right_count = 0)
      : left_count(left_count), right_count(right_count) {}

  /// The number of documents in the left bucket.
  uint32_t left_count;
  /// The number of documents in the right bucket.
  uint32_t right_count;
  /// Cached cost of moving a document from left to right bucket.
  double cached_cost_lr = 0;
  /// Cached cost of moving a document from right to left bucket.
  double cached_cost_rl = 0;
  /// Whether the cached costs must be recomputed.
  bool cache_is_invalid = true;
};
