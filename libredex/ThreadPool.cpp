/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ThreadPool.h"

namespace {

redex_thread_pool::ThreadPool s_threadpool;

} // anonymous namespace

namespace redex_thread_pool {

ThreadPool* ThreadPool::get_instance() { return &s_threadpool; }

boost::thread ThreadPool::create_thread(std::function<void()> bound_f) {
  boost::thread::attributes attrs;
  attrs.set_stack_size(8 * 1024 * 1024); // 8MB stack.
  auto bound_run = std::bind(&ThreadPool::run, this, std::move(bound_f));
  return boost::thread(attrs, std::move(bound_run));
}

} // namespace redex_thread_pool
