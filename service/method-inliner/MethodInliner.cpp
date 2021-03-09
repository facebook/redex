/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodInliner.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ClassHierarchy.h"
#include "ConfigFiles.h"
#include "Deleter.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Inliner.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Shrinker.h"
#include "Timer.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

/**
 * Collect all non virtual methods and make all small methods candidates
 * for inlining.
 */
std::unordered_set<DexMethod*> gather_non_virtual_methods(
    Scope& scope, const mog::Graph* method_override_graph) {
  // trace counter
  size_t all_methods = 0;
  size_t direct_methods = 0;
  size_t direct_no_code = 0;
  size_t non_virtual_no_code = 0;
  size_t clinit = 0;
  size_t init = 0;
  size_t static_methods = 0;
  size_t private_methods = 0;
  size_t dont_strip = 0;
  size_t non_virt_dont_strip = 0;
  size_t non_virt_methods = 0;

  // collect all non virtual methods (dmethods and vmethods)
  std::unordered_set<DexMethod*> methods;
  walk::methods(scope, [&](DexMethod* method) {
    all_methods++;
    if (method->is_virtual()) return;

    auto code = method->get_code();
    bool dont_inline = code == nullptr;

    direct_methods++;
    if (code == nullptr) direct_no_code++;
    if (method::is_constructor(method)) {
      (is_static(method)) ? clinit++ : init++;
      if (method::is_clinit(method)) {
        dont_inline = true;
      }
    } else {
      (is_static(method)) ? static_methods++ : private_methods++;
    }

    if (dont_inline) return;

    methods.insert(method);
  });
  if (method_override_graph) {
    auto non_virtual =
        mog::get_non_true_virtuals(*method_override_graph, scope);
    non_virt_methods = non_virtual.size();
    for (const auto& vmeth : non_virtual) {
      auto code = vmeth->get_code();
      if (code == nullptr) {
        non_virtual_no_code++;
        continue;
      }
      methods.insert(vmeth);
    }
  }

  TRACE(INLINE, 2, "All methods count: %ld", all_methods);
  TRACE(INLINE, 2, "Direct methods count: %ld", direct_methods);
  TRACE(INLINE, 2, "Virtual methods count: %ld", all_methods - direct_methods);
  TRACE(INLINE, 2, "Direct methods no code: %ld", direct_no_code);
  TRACE(INLINE, 2, "Direct methods with code: %ld",
        direct_methods - direct_no_code);
  TRACE(INLINE, 2, "Constructors with or without code: %ld", init);
  TRACE(INLINE, 2, "Static constructors: %ld", clinit);
  TRACE(INLINE, 2, "Static methods: %ld", static_methods);
  TRACE(INLINE, 2, "Private methods: %ld", private_methods);
  TRACE(INLINE, 2, "Virtual methods non virtual count: %ld", non_virt_methods);
  TRACE(INLINE, 2, "Non virtual no code count: %ld", non_virtual_no_code);
  TRACE(INLINE, 2, "Non virtual no strip count: %ld", non_virt_dont_strip);
  TRACE(INLINE, 2, "Don't strip inlinable methods count: %ld", dont_strip);
  return methods;
}

/**
 * Get a map of method -> implementation method that hold the same
 * implementation as the method would perform at run time.
 * So if a abtract method have multiple implementor but they all have the same
 * implementation, we can have a mapping between the abstract method and
 * one of its implementor.
 */
std::unordered_map<const DexMethod*, DexMethod*> get_same_implementation_map(
    const Scope& scope,
    const mog::Graph& method_override_graph,
    std::unordered_map<const DexMethod*, size_t>* same_method_implementations) {
  std::unordered_map<const DexMethod*, DexMethod*> method_to_implementations;
  walk::methods(scope, [&](DexMethod* method) {
    if (method->is_external() || root(method) || !method->is_virtual() ||
        (!method->get_code() && !is_abstract(method))) {
      return;
    }
    // Why can_rename? To mirror what VirtualRenamer looks at.
    if (is_interface(type_class(method->get_class())) &&
        (root(method) || !can_rename(method))) {
      // We cannot rule out that there are dynamically added classes, possibly
      // even created at runtime via Proxy.newProxyInstance, that override this
      // method. So we assume the worst.
      return;
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(method_override_graph, method);
    if (overriding_methods.empty()) {
      return;
    }
    // Filter out methods without IRCode.
    std::set<const DexMethod*, dexmethods_comparator> filtered_methods;
    for (auto overriding_method : overriding_methods) {
      if (is_abstract(overriding_method)) {
        continue;
      }
      if (!overriding_method->get_code() || root(overriding_method)) {
        // If the method is not abstract method and it doesn't have
        // implementation or is root, we bail out.
        return;
      }
      filtered_methods.emplace(overriding_method);
    }
    if (filtered_methods.empty()) {
      return;
    }
    if (method->get_code()) {
      filtered_methods.emplace(method);
    }

    // If all methods have the same implementation we create mapping between
    // methods and their representative implementation.
    auto* comparing_method = const_cast<DexMethod*>(*filtered_methods.begin());
    auto compare_method_ir = [&](const DexMethod* current) -> bool {
      return const_cast<DexMethod*>(current)->get_code()->structural_equals(
          *(comparing_method->get_code()));
    };
    if (std::all_of(std::next(filtered_methods.begin()), filtered_methods.end(),
                    compare_method_ir)) {
      auto update_method_to_implementations =
          [&](const DexMethod* method_to_update,
              DexMethod* representative_method) {
            method_to_implementations[method_to_update] = representative_method;
            if (filtered_methods.size() > 1) {
              auto& count = (*same_method_implementations)[method_to_update];
              count = std::max(count, filtered_methods.size());
            }
          };
      update_method_to_implementations(method, comparing_method);
      for (auto overriding_method : overriding_methods) {
        update_method_to_implementations(overriding_method, comparing_method);
      }
    }
  });
  return method_to_implementations;
}

using CallerInsns =
    std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>;
/**
 * Gather candidates of true virtual methods that can be inlined and their
 * call site in true_virtual_callers.
 * A true virtual method can be inlined to its callsite if the callsite can
 * be resolved to only one method implementation deterministically.
 * We are currently ruling out candidates that access field/methods or
 * return an object type.
 */
void gather_true_virtual_methods(
    const mog::Graph& method_override_graph,
    const Scope& scope,
    CalleeCallerInsns* true_virtual_callers,
    std::unordered_set<DexMethod*>* methods,
    std::unordered_map<const DexMethod*, size_t>* same_method_implementations) {
  auto non_virtual = mog::get_non_true_virtuals(method_override_graph, scope);
  auto same_implementation_map = get_same_implementation_map(
      scope, method_override_graph, same_method_implementations);
  std::unordered_set<DexMethod*> non_virtual_set{non_virtual.begin(),
                                                 non_virtual.end()};
  // Add mapping from callee to monomorphic callsites.
  auto update_monomorphic_callsite =
      [](DexMethod* caller, IRInstruction* callsite, DexMethod* callee,
         ConcurrentMap<DexMethod*, CallerInsns>* meth_caller) {
        if (!callee->get_code()) {
          return;
        }
        meth_caller->update(
            callee, [&](const DexMethod*, CallerInsns& m, bool /* exists */) {
              m[caller].emplace(callsite);
            });
      };

  ConcurrentMap<DexMethod*, CallerInsns> meth_caller;
  walk::parallel::code(scope, [&non_virtual_set, &method_override_graph,
                               &meth_caller, &update_monomorphic_callsite,
                               &same_implementation_map](DexMethod* method,
                                                         IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (insn->opcode() != OPCODE_INVOKE_VIRTUAL &&
          insn->opcode() != OPCODE_INVOKE_INTERFACE) {
        continue;
      }
      auto insn_method = insn->get_method();
      auto callee = resolve_method(insn_method, opcode_to_search(insn));
      if (callee == nullptr) {
        // There are some invoke-virtual call on methods whose def are
        // actually in interface.
        callee = resolve_method(insn->get_method(), MethodSearch::Interface);
      }
      if (callee == nullptr) {
        continue;
      }
      if (non_virtual_set.count(callee) != 0) {
        // Not true virtual, no need to continue;
        continue;
      }
      always_assert_log(callee->is_def(), "Resolved method not def %s",
                        SHOW(callee));
      if (same_implementation_map.count(callee)) {
        // We can find the resolved callee in same_implementation_map,
        // just use that piece of info because we know the implementors are all
        // the same
        update_monomorphic_callsite(
            method, insn, same_implementation_map[callee], &meth_caller);
        continue;
      }
      const auto& overriding_methods =
          mog::get_overriding_methods(method_override_graph, callee);
      if (!callee->is_external()) {
        if (overriding_methods.empty()) {
          // There is no override for this method
          update_monomorphic_callsite(
              method, insn, static_cast<DexMethod*>(callee), &meth_caller);
        } else if (is_abstract(callee) && overriding_methods.size() == 1) {
          // The method is an abstract method, the only override is its
          // implementation.
          auto implementing_method =
              const_cast<DexMethod*>(*(overriding_methods.begin()));

          update_monomorphic_callsite(method, insn, implementing_method,
                                      &meth_caller);
        }
      }
    }
  });

  // Post processing candidates.
  std::for_each(meth_caller.begin(), meth_caller.end(), [&](const auto& pair) {
    DexMethod* callee = pair.first;
    auto& caller_to_invocations = pair.second;
    always_assert_log(methods->count(callee) == 0, "%s\n", SHOW(callee));
    always_assert(callee->get_code());
    // Not considering candidates that accessed type in their method body
    // or returning a non primitive type.
    if (!type::is_primitive(type::get_element_type_if_array(
            callee->get_proto()->get_rtype()))) {
      return;
    }
    for (auto& mie : InstructionIterable(callee->get_code())) {
      auto insn = mie.insn;
      if (insn->has_type() || insn->has_method() || insn->has_field()) {
        return;
      }
    }
    methods->insert(callee);
    (*true_virtual_callers)[callee] = caller_to_invocations;
  });
}

} // namespace

namespace inliner {
void run_inliner(DexStoresVector& stores,
                 PassManager& mgr,
                 ConfigFiles& conf,
                 bool intra_dex /* false */,
                 InlineForSpeed* inline_for_speed) {
  if (mgr.no_proguard_rules()) {
    TRACE(INLINE, 1,
          "MethodInlinePass not run because no ProGuard configuration was "
          "provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  CalleeCallerInsns true_virtual_callers;
  // Gather all inlinable candidates.
  auto inliner_config = conf.get_inliner_config();
  if (intra_dex) {
    inliner_config.apply_intradex_allowlist();
  }

  if (inline_for_speed != nullptr) {
    inliner_config.shrink_other_methods = false;
  }

  inliner_config.unique_inlined_registers = false;

  std::unique_ptr<const mog::Graph> method_override_graph;
  if (inliner_config.virtual_inline) {
    method_override_graph = mog::build_graph(scope);
  }

  auto methods = gather_non_virtual_methods(scope, method_override_graph.get());

  // The methods list computed above includes all constructors, regardless of
  // whether it's safe to inline them or not. We'll let the inliner decide
  // what to do with constructors.
  bool analyze_and_prune_inits = true;

  std::unordered_map<const DexMethod*, size_t> same_method_implementations;
  if (inliner_config.virtual_inline && inliner_config.true_virtual_inline) {
    gather_true_virtual_methods(*method_override_graph, scope,
                                &true_virtual_callers, &methods,
                                &same_method_implementations);
  }
  // keep a map from refs to defs or nullptr if no method was found
  ConcurrentMethodRefCache concurrent_resolved_refs;
  auto concurrent_resolver = [&concurrent_resolved_refs](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolved_refs);
  };
  if (inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
      code.build_cfg(/* editable */ true);
    });
    inliner_config.shrinker.analyze_constructors =
        inliner_config.shrinker.run_const_prop;
  }

  // inline candidates
  MultiMethodInliner inliner(scope, stores, methods, concurrent_resolver,
                             inliner_config, intra_dex ? IntraDex : InterDex,
                             true_virtual_callers, inline_for_speed,
                             &same_method_implementations,
                             analyze_and_prune_inits, conf.get_pure_methods());
  inliner.inline_methods();

  if (inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope,
                         [](DexMethod*, IRCode& code) { code.clear_cfg(); });
  }

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t inlined_init_count = 0;
  for (DexMethod* m : inlined) {
    if (method::is_init(m)) {
      inlined_init_count++;
    }
  }

  // Do not erase true virtual methods that are inlined because we are only
  // inlining callsites that are monomorphic, for polymorphic callsite we
  // didn't inline, but in run time the callsite may still be resolved to
  // those methods that are inlined. We are relying on RMU to clean up
  // true virtual methods that are not referenced.
  for (const auto& pair : true_virtual_callers) {
    inlined.erase(pair.first);
  }
  ConcurrentSet<DexMethod*>& delayed_make_static =
      inliner.get_delayed_make_static();
  size_t deleted =
      delete_methods(scope, inlined, delayed_make_static, concurrent_resolver);

  TRACE(INLINE, 3, "recursive %ld", inliner.get_info().recursive);
  TRACE(INLINE, 3, "max_call_stack_depth %ld",
        inliner.get_info().max_call_stack_depth);
  TRACE(INLINE, 3, "waited seconds %ld", inliner.get_info().waited_seconds);
  TRACE(INLINE, 3, "blocklisted meths %ld",
        (size_t)inliner.get_info().blocklisted);
  TRACE(INLINE, 3, "virtualizing methods %ld",
        (size_t)inliner.get_info().need_vmethod);
  TRACE(INLINE, 3, "invoke super %ld", (size_t)inliner.get_info().invoke_super);
  TRACE(INLINE, 3, "escaped virtual %ld",
        (size_t)inliner.get_info().escaped_virtual);
  TRACE(INLINE, 3, "known non public virtual %ld",
        (size_t)inliner.get_info().non_pub_virtual);
  TRACE(INLINE, 3, "non public ctor %ld",
        (size_t)inliner.get_info().non_pub_ctor);
  TRACE(INLINE, 3, "unknown field %ld",
        (size_t)inliner.get_info().escaped_field);
  TRACE(INLINE, 3, "non public field %ld",
        (size_t)inliner.get_info().non_pub_field);
  TRACE(INLINE, 3, "throws %ld", (size_t)inliner.get_info().throws);
  TRACE(INLINE, 3, "multiple returns %ld",
        (size_t)inliner.get_info().multi_ret);
  TRACE(INLINE, 3, "references cross stores %ld",
        (size_t)inliner.get_info().cross_store);
  TRACE(INLINE, 3, "not found %ld", (size_t)inliner.get_info().not_found);
  TRACE(INLINE, 3, "caller too large %ld",
        (size_t)inliner.get_info().caller_too_large);
  TRACE(INLINE, 3, "inlined ctors %zu", inlined_init_count);
  TRACE(INLINE, 1, "%ld inlined calls over %ld methods and %ld methods removed",
        (size_t)inliner.get_info().calls_inlined, inlined_count, deleted);

  const auto& shrinker = inliner.get_shrinker();
  mgr.incr_metric("recursive", inliner.get_info().recursive);
  mgr.incr_metric("max_call_stack_depth",
                  inliner.get_info().max_call_stack_depth);
  mgr.incr_metric("caller_too_large", inliner.get_info().caller_too_large);
  mgr.incr_metric("inlined_init_count", inlined_init_count);
  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("calls_not_inlinable",
                  inliner.get_info().calls_not_inlinable);
  mgr.incr_metric("intermediate_shrinkings",
                  inliner.get_info().intermediate_shrinkings);
  mgr.incr_metric("calls_not_inlined", inliner.get_info().calls_not_inlined);
  mgr.incr_metric("methods_removed", deleted);
  mgr.incr_metric("escaped_virtual", inliner.get_info().escaped_virtual);
  mgr.incr_metric("unresolved_methods", inliner.get_info().unresolved_methods);
  mgr.incr_metric("known_public_methods",
                  inliner.get_info().known_public_methods);
  mgr.incr_metric("constant_invoke_callers_analyzed",
                  inliner.get_info().constant_invoke_callers_analyzed);
  mgr.incr_metric(
      "constant_invoke_callers_unreachable_blocks",
      inliner.get_info().constant_invoke_callers_unreachable_blocks);
  mgr.incr_metric("constant_invoke_callees_analyzed",
                  inliner.get_info().constant_invoke_callees_analyzed);
  mgr.incr_metric(
      "constant_invoke_callees_unreachable_blocks",
      inliner.get_info().constant_invoke_callees_unreachable_blocks);
  mgr.incr_metric("critical_path_length",
                  inliner.get_info().critical_path_length);
  mgr.incr_metric("methods_shrunk", shrinker.get_methods_shrunk());
  mgr.incr_metric("callers", inliner.get_callers());
  mgr.incr_metric("delayed_shrinking_callees",
                  inliner.get_delayed_shrinking_callees());
  mgr.incr_metric("instructions_eliminated_const_prop",
                  shrinker.get_const_prop_stats().branches_removed +
                      shrinker.get_const_prop_stats().branches_forwarded +
                      shrinker.get_const_prop_stats().materialized_consts +
                      shrinker.get_const_prop_stats().added_param_const +
                      shrinker.get_const_prop_stats().throws);
  mgr.incr_metric("instructions_eliminated_cse",
                  shrinker.get_cse_stats().instructions_eliminated);
  mgr.incr_metric("instructions_eliminated_copy_prop",
                  shrinker.get_copy_prop_stats().moves_eliminated);
  mgr.incr_metric(
      "instructions_eliminated_localdce",
      shrinker.get_local_dce_stats().dead_instruction_count +
          shrinker.get_local_dce_stats().unreachable_instruction_count);
  mgr.incr_metric("blocks_eliminated_by_dedup_blocks",
                  shrinker.get_dedup_blocks_stats().blocks_removed);
  mgr.incr_metric("methods_reg_alloced", shrinker.get_methods_reg_alloced());

  // Expose the shrinking timers as Timers.
  Timer::add_timer("Inliner.Shrinking.ConstantPropagation",
                   shrinker.get_const_prop_seconds());
  Timer::add_timer("Inliner.Shrinking.CSE", shrinker.get_cse_seconds());
  Timer::add_timer("Inliner.Shrinking.CopyPropagation",
                   shrinker.get_copy_prop_seconds());
  Timer::add_timer("Inliner.Shrinking.LocalDCE",
                   shrinker.get_local_dce_seconds());
  Timer::add_timer("Inliner.Shrinking.DedupBlocks",
                   shrinker.get_dedup_blocks_seconds());
  Timer::add_timer("Inliner.Shrinking.RegAlloc",
                   shrinker.get_reg_alloc_seconds());
}
} // namespace inliner
