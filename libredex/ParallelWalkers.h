/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <functional>
#include <thread>
#include <vector>

#include "ControlFlow.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "Match.h"
#include "Transform.h"
#include "WorkQueue.h"

/**
 * Walk all methods of all classes defined in 'scope' calling back
 * the walker function in parallel.  Make sure all global
 * information needed is copied locally per thread using DataInitializerFn.
 */
template <class T,
          class Data,
          class Output,
          class MethodWalkerFn = Output(Data&, DexMethod*),
          class OutputReducerFn = Output(Output, Output),
          class DataInitializerFn = Data(int)>
Output walk_methods_parallel(const T& scope,
                             MethodWalkerFn walker,
                             OutputReducerFn reducer,
                             DataInitializerFn data_initializer,
                             const Output& init = Output()) {
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
      std::thread::hardware_concurrency());

  for (const auto& cls : scope) {
    wq.add_item(cls);
  };
  return wq.run_all();
}
