/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

/*
 * Question: What happens when you alot yourself 30 minutes
 * to design and write a work-queue?
 * Answer: You're looking at it.
 *
 * This is a BatchWork (TM) type of work queue.  The work is
 * dispatched blocking the submitter of the work.  Lame, I know,
 * but good enough for now.
 *
 */

typedef void (*work_routine)(void*);
struct work_item {
  work_routine function;
  void* arg;
};

struct per_thread {
  work_item* wi __attribute__((aligned(64)));
  int next;
  int last;
  std::mutex lock;
  pthread_t thread;
  int thread_num;
};

class WorkQueue {
 private:
  static per_thread* s_per_thread;
  static std::condition_variable s_completion;
  static std::condition_variable s_work_ready;
  static std::mutex s_lock;
  static std::mutex s_work_running;
  static int s_threads_complete;
  static void* worker_thread(void* priv);
  static bool steal_work(per_thread* self);

 public:
  WorkQueue();

  void run_work_items(work_item* witems, int count);
};
