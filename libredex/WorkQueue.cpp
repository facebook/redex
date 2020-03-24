/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

#include <iostream>

#include "Debug.h"

namespace redex_workqueue_impl {

void redex_queue_exception_handler(std::exception& e) {
  print_stack_trace(std::cerr, e);
}

} // namespace redex_workqueue_impl
