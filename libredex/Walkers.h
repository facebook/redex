/**
 * Copyright (c) 2016-present, Facebook, Inc.
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
 * A collection of methods useful for iterating over elements of DexClasses.
 *
 * The name is intentionally lowercase. Think of this as a namespace with public
 * and private visibility.
 *
 * This is header-only because of the template arguments.
 */

class walk {
 public:
  // This is a "static class". Disallow construction.
  walk() = delete;
  ~walk() = delete;

  using ClassWalkerFn = const std::function<void(DexClass*)>&;
  using MethodWalkerFn = const std::function<void(DexMethod*)>&;
  using FieldWalkerFn = const std::function<void(DexField*)>&;
  using MethodFilterFn = const std::function<bool(DexMethod*)>&;
  using CodeWalkerFn = const std::function<void(DexMethod*, IRCode&)>&;
  using InsnWalkerFn = const std::function<void(DexMethod*, IRInstruction*)>&;
  using AnnotationWalkerFn = const std::function<void(DexAnnotation*)>&;
  using MatchingInBlockWalkerFn = const std::function<void(
      DexMethod*, cfg::Block*, const std::vector<IRInstruction*>&)>&;

  /**
   * Call walker on all classes in `classes`
   */
  template <class Classes>
  static void classes(Classes const& classes, ClassWalkerFn walker) {
    for (auto const& cls : classes) {
      walker(cls);
    }
  }

  /**
   * Call walker on all methods defined in `classes`
   */
  template <class Classes>
  static void methods(const Classes& classes, MethodWalkerFn walker) {
    for (const auto& cls : classes) {
      iterate_methods(cls, walker);
    };
  }

  /**
   * Call `walker` on all fields defined in `classes`
   */
  template <class Classes>
  static void fields(const Classes& classes, FieldWalkerFn walker) {
    for (const auto& cls : classes) {
      iterate_fields(cls, walker);
    };
  }

  /**
   * Call `walker` on the code of every method defined in classes that
   * satisfies the filter function
   */
  template <class Classes>
  static void code(const Classes& classes,
                   MethodFilterFn filter,
                   CodeWalkerFn walker) {
    for (const auto& cls : classes) {
      iterate_code(cls, filter, walker);
    };
  }

  /**
   * Same as `code()` but with a filter that accepts all methods
   */
  template <class Classes>
  static void code(const Classes& classes, CodeWalkerFn walker) {
    walk::code(classes, all_methods, walker);
  }

  /**
   * Call `walker` on every instruction in the code of every method defined in
   * `classes` that satisfies the filter function.
   */
  template <class Classes>
  static void opcodes(const Classes& classes,
                      MethodFilterFn filter,
                      InsnWalkerFn walker) {
    for (const auto& cls : classes) {
      iterate_opcodes(cls, filter, walker);
    };
  }

  /**
   * Same as `opcodes()` but with a filter that accepts all methods
   */
  template <class Classes>
  static void opcodes(const Classes& classes, InsnWalkerFn walker) {
    walk::opcodes(classes, all_methods, walker);
  }

  /**
   * Call `walker` on every annotation on the classes (and its fields, methods,
   * and method parameters) defined in `classes`
   */
  template <class Classes>
  static void annotations(const Classes& classes, AnnotationWalkerFn walker) {
    for (auto& cls : classes) {
      iterate_annotations(cls, walker);
    }
  }

  /**
   * Visit sequences of opcodes that satisfy the give matcher.
   *
   * Example
   * -------
   *
   * The following code (taken from ReachableClasses) visits all opcode
   * sequences that match the the form "const-string, invoke-static" where
   * invoke-static is specifically invoking Class.forName that takes one
   * argument.
   *
   * In the walker callback, you can see that the opcodes are further inspected
   * to ensure that the register that const-string loads into is actually the
   * register that is referenced by invoke-static. (Without captures, this can't
   * be expressed in the matcher language alone)
   *
   * The opcodes that match are passed in as a pointer to an array of
   * IRInstruction pointers. The size of the array is passed in as 'n'.
   *
   * Example Code
   * ------------
   *
   * auto match = std::make_tuple(
   *     m::const_string(),
   *     m::invoke_static(
   *         m::opcode_method(m::named<DexMethod>("forName") &&
   *                          m::on_class<DexMethod>("Ljava/lang/Class;")) &&
   *         m::has_n_args(1)));
   *
   * match_opcodes(classes,
   *               match,
   *               [&](DexMethod* m, const std::vector<IRInstruction*>& insns) {
   *                 auto const_string = insns[0];
   *                 auto invoke_static = insns[1];
   *                 // Make sure that the registers agree
   *                 if (const_string->dest() == invoke_static->src(0)) {
   *                   ...
   *                 }
   *               });
   */

  template <class Classes,
            typename Predicate,
            size_t N = std::tuple_size<Predicate>::value,
            typename Walker = void(DexMethod*,
                                   const std::vector<IRInstruction*>&)>
  static void matching_opcodes(const Classes& classes,
                               const Predicate& predicate,
                               const Walker& walker,
                               MethodFilterFn filter = all_methods) {
    for (const auto& cls : classes) {
      iterate_matching(cls, predicate, walker, filter);
    }
  }

  /**
   * walker that respects basic block boundaries.
   *
   * It will not match a pattern that crosses block boundaries
   */
  template <class Classes,
            typename Predicate,
            size_t N = std::tuple_size<Predicate>::value>
  static void matching_opcodes_in_block(const Classes& classes,
                                        const Predicate& predicate,
                                        MatchingInBlockWalkerFn walker,
                                        MethodFilterFn filter = all_methods) {
    for (const auto& cls : classes) {
      iterate_matching_block(cls, predicate, walker, filter);
    }
  }

  template <typename Predicate, size_t N = std::tuple_size<Predicate>::value>
  static void matching_opcodes_in_block(DexMethod& method,
                                        const Predicate& predicate,
                                        MatchingInBlockWalkerFn walker) {
    always_assert(method.get_code() != nullptr);
    iterate_matching_block_worker(
        method, *method.get_code(), predicate, walker);
  }

 private:
  /* The iterate_* methods take a single dexclass and call the walker on the
   * elements requested. The reason that these are done on a class level is so
   * that we can share code with the `parallel::` methods.
   */

  static void iterate_methods(const DexClass* cls, MethodWalkerFn walker) {
    for (auto dmethod : cls->get_dmethods()) {
      TraceContext context(dmethod->get_deobfuscated_name());
      walker(dmethod);
    }
    for (auto vmethod : cls->get_vmethods()) {
      TraceContext context(vmethod->get_deobfuscated_name());
      walker(vmethod);
    }
  }

  static void iterate_fields(const DexClass* cls, FieldWalkerFn walker) {
    for (auto ifield : cls->get_ifields()) {
      walker(ifield);
    }
    for (auto sfield : cls->get_sfields()) {
      walker(sfield);
    }
  }

  static void iterate_code(const DexClass* cls,
                           MethodFilterFn filter,
                           CodeWalkerFn walker) {
    iterate_methods(cls, [&filter, &walker](DexMethod* m) {
      if (filter(m)) {
        auto code = m->get_code();
        if (code) {
          walker(m, *code);
        }
      }
    });
  }

  static void iterate_opcodes(const DexClass* cls,
                              MethodFilterFn filter,
                              InsnWalkerFn walker) {
    iterate_code(cls, filter, [&walker](DexMethod* m, IRCode& code) {
      for (const auto& mie : InstructionIterable(code)) {
        walker(m, mie.insn);
      }
    });
  }

  static void iterate_annotations(DexClass* cls, AnnotationWalkerFn walker) {
    call_annotation_walker(cls, walker);
    iterate_fields(cls, [&walker](DexField* field) {
      call_annotation_walker(field, walker);
    });
    iterate_methods(cls, [&walker](DexMethod* method) {
      call_annotation_walker(method, walker);
      const auto& param_anno = method->get_param_anno();
      if (!param_anno) return;
      for (const auto& it : *param_anno) {
        auto& anno_list = it.second->get_annotations();
        for (auto& anno : anno_list) {
          walker(anno);
        }
      }
    });
  }

  template <class T>
  static void call_annotation_walker(T* dex_thingy, AnnotationWalkerFn walker) {
    const auto& anno_set = dex_thingy->get_anno_set();
    if (!anno_set) return;
    auto& anno_list = anno_set->get_annotations();
    for (auto& anno : anno_list) {
      walker(anno);
    }
  }

  template <typename Predicate,
            size_t N = std::tuple_size<Predicate>::value,
            typename Walker = void(DexMethod*,
                                   const std::vector<IRInstruction*>&)>
  static void iterate_matching_worker(DexMethod& m,
                                      IRCode& ir_code,
                                      const Predicate& predicate,
                                      const Walker& walker) {
    std::vector<IRInstruction*> insns;
    for (MethodItemEntry& mie : InstructionIterable(ir_code)) {
      insns.emplace_back(mie.insn);
    }

    std::vector<std::vector<IRInstruction*>> matches;
    m::find_matches(insns, predicate, matches);
    for (const std::vector<IRInstruction*>& matching_insns : matches) {
      walker(&m, matching_insns);
    }
  }

  template <typename Predicate,
            size_t N = std::tuple_size<Predicate>::value,
            typename Walker = void(DexMethod*,
                                   const std::vector<IRInstruction*>&)>
  static void iterate_matching(DexClass* cls,
                               const Predicate& predicate,
                               const Walker& walker,
                               MethodFilterFn filter = all_methods) {
    iterate_code(
        cls, filter, [&predicate, &walker](DexMethod* m, IRCode& ir_code) {
          iterate_matching_worker(*m, ir_code, predicate, walker);
        });
  }

  template <typename Predicate, size_t N = std::tuple_size<Predicate>::value>
  static void iterate_matching_block_worker(DexMethod& m,
                                            IRCode& ir_code,
                                            const Predicate& predicate,
                                            MatchingInBlockWalkerFn walker) {
    std::vector<std::pair<cfg::Block*, std::vector<IRInstruction*>>>
        block_matches;
    ir_code.build_cfg();
    for (cfg::Block* block : ir_code.cfg().blocks()) {
      std::vector<std::vector<IRInstruction*>> method_matches;
      std::vector<IRInstruction*> insns;
      for (const auto& mie : InstructionIterable(block)) {
        insns.emplace_back(mie.insn);
      }
      m::find_matches(insns, predicate, method_matches);
      for (auto& matched_insns : method_matches) {
        block_matches.emplace_back(block, std::move(matched_insns));
      }
    }

    for (const auto& match : block_matches) {
      walker(&m, match.first, match.second);
    }
  }

  template <typename Predicate, size_t N = std::tuple_size<Predicate>::value>
  static void iterate_matching_block(DexClass* cls,
                                     const Predicate& predicate,
                                     MatchingInBlockWalkerFn walker,
                                     MethodFilterFn filter = all_methods) {
    iterate_code(
        cls, filter, [&predicate, &walker](DexMethod* m, IRCode& ir_code) {
          iterate_matching_block_worker(*m, ir_code, predicate, walker);
        });
  }

  static constexpr bool all_methods(DexMethod*) { return true; }

 public:
  /**
   * The parallel:: methods have very similar signatures (and names) to their
   * sequential counterparts.
   * The unit of parallelization is a DexClass. The reason is that we don't want
   * to create too many tasks on the WorkQueue, paying the overhead for each.
   */
  class parallel {
   public:
    parallel() = delete;
    ~parallel() = delete;

    /**
     * Call walker on all classes in `classes` in parallel.
     */
    template <class Classes>
    static void classes(Classes const& classes,
                        ClassWalkerFn walker,
                        size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>( // over-parallelized maybe
          [&walker](DexClass* cls) { walker(cls); },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Call `walker` on all methods in `classes` in parallel.
     */
    template <class Classes>
    static void methods(const Classes& classes,
                        MethodWalkerFn walker,
                        size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&walker](DexClass* cls) { walk::iterate_methods(cls, walker); },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Call `walker` on all methods in `classes` in parallel. Then combine the
     * Output with `reducer` function. Make sure all global information needed
     * is copied locally per thread using DataInitializerFn.
     */
    template <class Data,
              class Output,
              class Classes,
              class MethodWalkerFn = Output(Data&, DexMethod*),
              class OutputReducerFn = Output(Output, Output),
              class DataInitializerFn = Data(int)>
    Output static reduce_methods(const Classes& classes,
                                 MethodWalkerFn walker,
                                 OutputReducerFn reducer,
                                 DataInitializerFn data_initializer,
                                 const Output& init = Output(),
                                 size_t num_threads = default_num_threads()) {
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

      for (const auto& cls : classes) {
        wq.add_item(cls);
      };
      return wq.run_all();
    }

    /**
     * Call `walker` on all fields in `classes` in parallel.
     */
    template <class Classes>
    static void fields(const Classes& classes,
                       FieldWalkerFn walker,
                       size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&walker](DexClass* cls) { walk::iterate_fields(cls, walker); },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Call `walker` on all code (of methods approved by `filter`) in `classes`
     * in parallel.
     */
    template <class Classes>
    static void code(const Classes& classes,
                     MethodFilterFn filter,
                     CodeWalkerFn walker,
                     size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&filter, &walker](DexClass* cls) {
            walk::iterate_code(cls, filter, walker);
          },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Same as `code()` but with a filter function that accepts all methods
     */
    template <class Classes>
    static void code(const Classes& classes,
                     CodeWalkerFn walker,
                     size_t num_threads = default_num_threads()) {
      walk::parallel::code(classes, all_methods, walker, num_threads);
    }

    /**
     * Call `walker` on all opcodes (of methods approved by `filter`) in
     * `classes` in parallel.
     */
    template <class Classes>
    static void opcodes(const Classes& classes,
                        MethodFilterFn filter,
                        InsnWalkerFn walker,
                        size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&filter, &walker](DexClass* cls) {
            walk::iterate_opcodes(cls, filter, walker);
          },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Same as `opcodes()` but with a filter function that accepts all methods
     */
    template <class Classes>
    static void opcodes(const Classes& classes,
                        InsnWalkerFn walker,
                        size_t num_threads = default_num_threads()) {
      walk::parallel::opcodes(classes, all_methods, walker, num_threads);
    }

    /**
     * Call `walker` on all annotations in `classes` in parallel.
     */
    template <class Classes>
    static void annotations(const Classes& classes,
                            AnnotationWalkerFn walker,
                            size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&walker](DexClass* cls) { walk::iterate_annotations(cls, walker); },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Call `walker` on all matching opcodes (according to `predicate`) in
     * `classes` in parallel.
     * This will match across basic block boundaries. So be careful!
     */
    template <class Classes,
              typename Predicate,
              size_t N = std::tuple_size<Predicate>::value,
              typename Walker = void(DexMethod*,
                                     const std::vector<IRInstruction*>&)>
    static void matching_opcodes(const Classes& classes,
                                 const Predicate& predicate,
                                 const Walker& walker,
                                 size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&predicate, &walker](DexClass* cls) {
            walk::iterate_matching(cls, predicate, walker);
          },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * Call `walker` on all matching opcodes (according to `predicate`) in
     * `classes` in parallel.
     * This will not match across basic block boundaries.
     */
    template <class Classes,
              typename Predicate,
              size_t N = std::tuple_size<Predicate>::value>
    static void matching_opcodes_in_block(
        const Classes& classes,
        const Predicate& predicate,
        MatchingInBlockWalkerFn walker,
        size_t num_threads = default_num_threads()) {
      auto wq = workqueue_foreach<DexClass*>(
          [&predicate, &walker](DexClass* cls) {
            walk::iterate_matching_block(cls, predicate, walker);
          },
          num_threads);
      run_all(wq, classes);
    }

    /**
     * This code usually runs on a processor with Hyperthreading, where the
     * number of physical cores is half the number of logical cores. Setting
     * num_threads to that number often gets us good results, so that's the
     * default.
     */
    static unsigned int default_num_threads() {
      unsigned int threads = std::thread::hardware_concurrency() / 2;
      return std::max(1u, threads);
    }

   private:
    template <class WQ, class Classes>
    static void run_all(WQ& wq, const Classes& classes) {
      for (const auto& cls : classes) {
        wq.add_item(cls);
      };
      wq.run_all();
    }
  };
};
