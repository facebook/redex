/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <pthread.h>

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

template<typename T>
struct WorkItem {
  void init(void (*fn)(T*), T* arg) {
    m_w.function = reinterpret_cast<work_routine>(fn);
    m_w.arg = reinterpret_cast<void*>(arg);
  }

 private:
  work_item m_w;
};  

struct per_thread {
  work_item* wi __attribute__((aligned(64)));
  int next;
  int last;
  pthread_mutex_t lock;
  pthread_t thread;
  int thread_num;
};

class WorkQueue {
 private:
  static per_thread* s_per_thread;
  static pthread_cond_t s_completion;
  static pthread_cond_t s_work_ready;
  static pthread_mutex_t s_lock;
  static pthread_mutex_t s_work_running;
  static int s_threads_complete;
  static void* worker_thread(void* priv);
  static bool steal_work(per_thread* self);

  void run_work_items(work_item* witems, int count);

 public:
  WorkQueue();
  
  template<typename T>
  void run_work_items(WorkItem<T>* witems, int count) {
    static_assert(sizeof(WorkItem<T>) == sizeof(work_item),
                  "WorkItem must be binary-compatible with work_item");
    run_work_items(reinterpret_cast<work_item*>(witems), count);
  }
};
