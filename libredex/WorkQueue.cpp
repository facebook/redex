/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

static thread_local int worker_id{redex_parallel::INVALID_ID};

namespace workqueue_impl {

void set_worker_id(int id) { worker_id = id; }

} // namespace workqueue_impl

namespace redex_parallel {

int get_worker_id() { return worker_id; }

} // namespace redex_parallel
