/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// We for now need a larger stack size than the default, and on Mac OS
// this is the only way (or pthreads directly), as `ulimit -s` does not
// apply to non-main threads.
#include <boost/thread.hpp>

#include <sparta/WorkQueue.h>

namespace redex_thread_pool {

class ThreadPool : public sparta::ThreadPool<boost::thread> {
 public:
  static ThreadPool* get_instance();

  static void create();

  static void destroy();

 protected:
  boost::thread create_thread(std::function<void()> bound_f) override;
};

} // namespace redex_thread_pool
