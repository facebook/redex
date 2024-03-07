/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "DexClass.h"
#include "DexOutput.h"

/**
 * This object defines compression-conscious code placement.
 *
 * The primary goal of the placement is to collocate "similar" functions in the
 * ordering. Such functions, comprised of identical/similar instructions, can
 * be effectively encoded by a compression algorithm (e.g., LZ4 or ZLIB), which
 * leads to smaller compressed binaries.
 * We consider a proxy metric that correlates an ordering of the functions with
 * the compression achieved by a compression algorithm. It is "the number of
 * distinct k-mers in the byte stream within a window of specified size (64KB)".
 * A k-mer is a substring containing k characters; equivalently, this is k
 * contiguous bytes in the file. In our implementation, k=8, and thus a k-mer
 * contains 64 bits.
 * In order to create a function order minimizing the number of k-mers in a
 * window, we utilize the balanced graph partitioning algorithm; see the docs
 * below. The input data is represented by a bipartite graph; one part is
 * the input functions (called Documents), the other part is comprised of all
 * distinct k-mers in the dataset. The algorithm reorders the documents so as
 * to minimize the proxy metric.
 */
class MethodSimilarityCompressionConsciousOrderer {
 private:
  // The content of the method (a sequence of bytes representing the method).
  std::vector<uint8_t> get_encoded_method_content(
      DexMethod* meth,
      std::unique_ptr<DexOutputIdx>& dodx,
      std::unique_ptr<uint8_t[]>& output);

 public:
  void order(std::vector<DexMethod*>& methods, GatheredTypes* m_gtypes);
};
