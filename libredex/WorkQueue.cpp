/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "WorkQueue.h"

#include <stdio.h>

#include "Trace.h"

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
constexpr int WORKER_THREADS = 4;

per_thread* WorkQueue::s_per_thread = nullptr;
pthread_cond_t WorkQueue::s_completion = PTHREAD_COND_INITIALIZER;
pthread_cond_t WorkQueue::s_work_ready = PTHREAD_COND_INITIALIZER;
pthread_mutex_t WorkQueue::s_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t WorkQueue::s_work_running = PTHREAD_MUTEX_INITIALIZER;
int WorkQueue::s_threads_complete;

inline int get_next_thread_num(int threadno) {
  threadno++;
  if (threadno == WORKER_THREADS) return 0;
  return threadno;
}

bool WorkQueue::steal_work(per_thread* self) {
  int threadno = get_next_thread_num(self->thread_num);
  while (self->thread_num != threadno) {
    per_thread* target = &s_per_thread[threadno];
    pthread_mutex_lock(&target->lock);
    int stealcount = (target->next - target->last) / 2;
    if (stealcount > 0) {
      // Found work!
      work_item* wi = target->wi;
      int next = target->next;
      int last = target->last - stealcount;
      target->next = last;
      pthread_mutex_unlock(&target->lock);
      pthread_mutex_lock(&self->lock);
      self->wi = wi;
      self->next = next;
      self->last = last;
      pthread_mutex_unlock(&self->lock);
      return true;
    }
    pthread_mutex_unlock(&target->lock);
    threadno = get_next_thread_num(threadno);
  }
  return false;
}

void* WorkQueue::worker_thread(void* priv) {
  per_thread* self = (per_thread*)priv;
  while (1) {
    pthread_mutex_lock(&self->lock);
    if (self->next < self->last) {
      work_item* todo = &self->wi[self->next++];
      pthread_mutex_unlock(&self->lock);
      todo->function(todo->arg);
      continue;
    }
    pthread_mutex_unlock(&self->lock);
    if (steal_work(self)) continue;
    /* Nothing to do..., wait for it. */
    pthread_mutex_lock(&s_lock);
    s_threads_complete++;
    if (s_threads_complete == WORKER_THREADS) {
      pthread_cond_signal(&s_completion);
    }
    pthread_cond_wait(&s_work_ready, &s_lock);
    pthread_mutex_unlock(&s_lock);
  }
  return nullptr;
}

WorkQueue::WorkQueue() {
  pthread_mutex_lock(&s_lock);
  if (s_per_thread == nullptr) {
    s_per_thread = new per_thread[WORKER_THREADS];
    for (int i = 0; i < WORKER_THREADS; i++) {
      s_per_thread[i].wi = nullptr;
      s_per_thread[i].next = 0;
      s_per_thread[i].last = 0;
      s_per_thread[i].thread_num = i;
      pthread_mutex_init(&s_per_thread[i].lock, nullptr);
    }
    /* Steal work can peek at other threads work queues,
     * so we have to init all the work queues before
     * launching any threads.
     */
    for (int i = 0; i < WORKER_THREADS; i++) {
      pthread_create(
          &s_per_thread[i].thread, nullptr, &worker_thread, &s_per_thread[i]);
    }
  }
  pthread_mutex_unlock(&s_lock);
}

/* Caller owns memory for witems.  WorkQueue does not free it. */
void WorkQueue::run_work_items(work_item* witems, int count) {
  pthread_mutex_lock(&s_work_running);
  pthread_mutex_lock(&s_lock);
  while (s_threads_complete < WORKER_THREADS)
    pthread_cond_wait(&s_completion, &s_lock);
  if (witems != nullptr) {
    int per_thread_count = count / WORKER_THREADS;
    int sindex = 0;
    int i;
    for (i = 0; i < (WORKER_THREADS - 1); i++) {
      s_per_thread[i].wi = witems;
      s_per_thread[i].next = sindex;
      s_per_thread[i].last = sindex + per_thread_count;
      sindex += per_thread_count;
    }
    s_per_thread[i].wi = witems;
    s_per_thread[i].next = sindex;
    s_per_thread[i].last = count;
    s_threads_complete = 0;
    pthread_cond_broadcast(&s_work_ready);
    do {
      pthread_cond_wait(&s_completion, &s_lock);
    } while (s_threads_complete < WORKER_THREADS);
  }
  pthread_mutex_unlock(&s_lock);
  pthread_mutex_unlock(&s_work_running);
}
