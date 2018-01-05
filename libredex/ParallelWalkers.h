/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

#include "ControlFlow.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "IRCode.h"
#include "Match.h"
#include "WorkQueue.h"

/**
 * This code usually runs on a processor with Hyperthreading, where the number
 * of physical cores is half the number of logical cores. Setting num_threads
 * to that number often gets us good results, so that's the default.
 */
inline unsigned int walkers_default_num_threads() {
  unsigned int threads = std::thread::hardware_concurrency() / 2;
  return std::max(1u, threads);
}

/**
 * Walk all methods of all classes defined in 'scope' calling back
 * the walker function in parallel.  Make sure all global
 * information needed is copied locally per thread using DataInitializerFn.
 */
template <class Data,
          class Output,
          class Scope,
          class MethodWalkerFn = Output(Data&, DexMethod*),
          class OutputReducerFn = Output(Output, Output),
          class DataInitializerFn = Data(int)>
Output walk_methods_parallel(
    const Scope& scope,
    MethodWalkerFn walker,
    OutputReducerFn reducer,
    DataInitializerFn data_initializer,
    const Output& init = Output(),
    size_t num_threads = walkers_default_num_threads()) {
  auto wq = WorkQueue<DexClass*, Data, Output>(
      [&](Data& data, DexClass* cls) {
        Output out = init;
        for (auto dmethod : cls->get_dmethods()) {
          TraceContext context(dmethod->get_deobfuscated_name());
          out = reducer(out, walker(data, dmethod));
        }
        for (auto vmethod : cls->get_vmethods()) {
          TraceContext context(vmethod->get_deobfuscated_name());
          out = reducer(out, walker(data, vmethod));
        }
        return out;
      },
      reducer,
      data_initializer,
      num_threads);

  for (const auto& cls : scope) {
    wq.add_item(cls);
  };
  return wq.run_all();
}

// The simple version. Call `walker` on all methods in `scope` in parallel.
template <class Scope>
void walk_methods_parallel_simple(
    const Scope& scope,
    const std::function<void(DexMethod*)>& walker,
    size_t num_threads = walkers_default_num_threads()) {
  walk_methods_parallel<std::nullptr_t, std::nullptr_t, Scope>(
      scope,
      [&walker](std::nullptr_t, DexMethod* m) {
        walker(m);
        return nullptr;
      },
      [](std::nullptr_t, std::nullptr_t) { return nullptr; },
      [](int) { return nullptr; },
      nullptr,
      num_threads);
}
