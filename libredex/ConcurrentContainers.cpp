/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"

#include "WorkQueue.h"

namespace cc_impl {

void workqueue_run_for(size_t start,
                       size_t end,
                       const std::function<void(size_t)>& fn) {
  ::workqueue_run_for<size_t>(start, end, fn);
}

} // namespace cc_impl
