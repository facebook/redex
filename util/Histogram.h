/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace histogram {

// Default number of buckets the value range is split into.
constexpr size_t kDefaultNumBuckets = 20;
// Default width (in characters) of the longest bar.
constexpr size_t kDefaultBarWidth = 50;

// Renders an ASCII-art histogram of `values` as a multi-line string.
//
// The closed integer range [min(values), max(values)] is divided into up to
// `num_buckets` contiguous, equal-width buckets; each row shows the bucket's
// integer range, a bar, and the raw count. Bars are scaled to the busiest
// bucket over a `bar_width`-character field, rounding up so any non-empty
// bucket renders at least one character. Returns an empty string when `values`
// is empty.
std::string render_histogram(const std::vector<size_t>& values,
                             std::string_view title = "Distribution",
                             size_t num_buckets = kDefaultNumBuckets,
                             size_t bar_width = kDefaultBarWidth);

} // namespace histogram
