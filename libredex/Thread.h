/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/thread/thread.hpp>

// As far as Intel processors are concerned...
#define CACHE_LINE_SIZE 64

namespace redex_parallel {

/**
 * Redex uses the number of physical cores.
 */
inline unsigned int default_num_threads() {
  unsigned int threads = boost::thread::physical_concurrency();
  return std::max(1u, threads);
}

} // namespace redex_parallel
