/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "CheckCastAnalysis.h"

namespace check_casts {

namespace impl {

struct Stats {
  size_t removed_casts{0};
  size_t replaced_casts{0};
  size_t weakened_casts{0};
  Stats& operator+=(const Stats&);
};

Stats apply(DexMethod* method, const CheckCastReplacements& casts);

} // namespace impl

} // namespace check_casts
