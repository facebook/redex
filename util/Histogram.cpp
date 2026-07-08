/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Histogram.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

namespace histogram {

std::string render_histogram(const std::vector<size_t>& values,
                             std::string_view title,
                             size_t num_buckets,
                             size_t bar_width) {
  std::ostringstream os;
  if (values.empty() || num_buckets == 0) {
    return os.str();
  }

  size_t min_v = values.front();
  size_t max_v = values.front();
  for (size_t v : values) {
    min_v = std::min(min_v, v);
    max_v = std::max(max_v, v);
  }

  size_t span = max_v - min_v + 1;
  size_t width = (span + num_buckets - 1) / num_buckets; // ceil, >= 1
  size_t used = (span + width - 1) / width; // number of buckets, <= num_buckets
  std::vector<size_t> buckets(used, 0);
  for (size_t v : values) {
    buckets[(v - min_v) / width]++;
  }
  size_t max_bucket = *std::max_element(buckets.begin(), buckets.end());
  // Width of the count column: enough digits for the busiest bucket, so the
  // counts line up across all rows.
  size_t count_width = std::to_string(max_bucket).size();

  os << title << " (" << values.size() << " value(s), range [" << min_v << ", "
     << max_v << "], " << used << " bucket(s)):\n";
  for (size_t i = 0; i < used; i++) {
    size_t lo = min_v + i * width;
    size_t hi = std::min(lo + width - 1, max_v);
    size_t n = buckets[i];
    // Scale the bar to the busiest bucket; round up so any non-empty bucket is
    // visible.
    size_t bar = (max_bucket == 0 || n == 0)
                     ? 0
                     : (n * bar_width + max_bucket - 1) / max_bucket;
    os << "  [" << std::setw(4) << lo << ", " << std::setw(4) << hi << "] "
       << std::setw(static_cast<int>(count_width)) << n;
    if (bar > 0) {
      os << " " << std::string(bar, '#');
    }
    os << "\n";
  }
  return os.str();
}

} // namespace histogram
