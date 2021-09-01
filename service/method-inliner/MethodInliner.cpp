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
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "ScopedMetrics.h"
#include "Shrinker.h"
#include "StlUtil.h"
#include "Timer.h"
#include "Walkers.h"
#include "WorkQueue.h"

namespace mog = method_override_graph;

namespace {

static DexType* get_receiver_type_demand(DexType* callee_rtype,
                                         const live_range::Use& use) {
  switch (use.insn->opcode()) {
  case OPCODE_RETURN_OBJECT:
    always_assert(use.src_index == 0);
    return callee_rtype;
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_CHECK_CAST:
  case OPCODE_INSTANCE_OF:
    always_assert(use.src_index == 0);
    return type::java_lang_Object();
  case OPCODE_THROW:
    always_assert(use.src_index == 0);
    return type::java_lang_Throwable();
  case OPCODE_FILLED_NEW_ARRAY:
    return type::get_array_component_type(use.insn->get_type());
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    DexMethodRef* dex_method = use.insn->get_method();
    auto src_index = use.src_index;
    if (use.insn->opcode() != OPCODE_INVOKE_STATIC) {
      // The first argument is a reference to the object instance on which the
      // method is invoked.
      if (src_index-- == 0) {
        return dex_method->get_class();
      }
    }
    const auto& arg_types =
        dex_method->get_proto()->get_args()->get_type_list();
    return arg_types.at(src_index);
  }
  default:
    if (opcode::is_a_conditional_branch(use.insn->opcode())) {
      return type::java_lang_Object();
    }
    if (opcode::is_an_iget(use.insn->opcode())) {
      always_assert(use.src_index == 0);
      return use.insn->get_field()->get_class();
    }
    if (opcode::is_an_iput(use.insn->opcode()) ||
        opcode::is_an_sput(use.insn->opcode())) {
      if (use.src_index == 0) {
        return use.insn->get_field()->get_type();
      } else {
        always_assert(use.src_index == 1);
        return use.insn->get_field()->get_class();
      }
    }
    if (opcode::is_an_aput(use.insn->opcode())) {
      always_assert(use.src_index == 0);
      // TODO: Use type inference to figure out required array (component) type.
      // For now, we just play it save and give up.
      return nullptr;
    }
    not_reached_log(
        "Unsupported instruction {%s} in get_receiver_type_demand\n",
        SHOW(use.insn));
  }
}

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

struct SameImplementation {
  DexMethod* representative;
  std::vector<DexMethod*> methods;
};

/**
 * Get a map of method -> implementation method that hold the same
 * implementation as the method would perform at run time.
 * So if a abtract method have multiple implementor but they all have the same
 * implementation, we can have a mapping between the abstract method and
 * one of its implementor.
 */
std::unordered_map<const DexMethod*, std::shared_ptr<SameImplementation>>
get_same_implementation_map(const Scope& scope,
                            const mog::Graph& method_override_graph) {
  std::unordered_map<const DexMethod*, std::shared_ptr<SameImplementation>>
      method_to_implementations;
  walk::methods(scope, [&](DexMethod* method) {
    if (method->is_external() || !method->is_virtual() ||
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
    auto overriding_methods =
        mog::get_overriding_methods(method_override_graph, method);
    SameImplementation same_implementation{nullptr, {}};
    auto consider_method = [&](DexMethod* method) {
      always_assert(method->get_code());
      if (same_implementation.representative != nullptr &&
          !method->get_code()->structural_equals(
              *same_implementation.representative->get_code())) {
        return false;
      }
      if (same_implementation.representative == nullptr ||
          compare_dexmethods(method, same_implementation.representative)) {
        same_implementation.representative = method;
      }
      same_implementation.methods.push_back(method);
      return true;
    };
    for (auto overriding_method : overriding_methods) {
      if (is_abstract(overriding_method)) {
        continue;
      }
      if (!overriding_method->get_code()) {
        // If the method is not abstract method and it doesn't have
        // implementation, we bail out.
        return;
      }
      if (!consider_method(const_cast<DexMethod*>(overriding_method))) {
        return;
      }
    }
    if (method->get_code() && !consider_method(method)) {
      return;
    }
    if (same_implementation.methods.size() <= 1) {
      return;
    }

    // All methods have the same implementation, so we create mapping between
    // methods and their representative implementation.
    auto sp = std::make_shared<SameImplementation>(same_implementation);
    method_to_implementations.emplace(method, sp);
    for (auto overriding_method : overriding_methods) {
      method_to_implementations.emplace(overriding_method, sp);
    }
  });
  return method_to_implementations;
}

static DexType* reduce_type_demands(
    std::unique_ptr<std::unordered_set<DexType*>>& type_demands) {
  if (!type_demands) {
    return nullptr;
  }
  // remove less specific object types
  std20::erase_if(*type_demands, [&type_demands](auto u) {
    return std::find_if(type_demands->begin(), type_demands->end(),
                        [u](const DexType* t) {
                          return t != u && type::check_cast(t, u);
                        }) != type_demands->end();
  });
  return type_demands->size() == 1 ? *type_demands->begin() : nullptr;
}

bool can_have_unknown_implementations(const mog::Graph& method_override_graph,
                                      const DexMethod* method,
                                      bool consider_overridden_methods = true) {
  // Why can_rename? To mirror what VirtualRenamer looks at.
  if (method->is_external() || (is_interface(type_class(method->get_class())) &&
                                (root(method) || !can_rename(method)))) {
    // We cannot rule out that there are dynamically added classes, possibly
    // even created at runtime via Proxy.newProxyInstance, that override
    // this method. So we assume the worst.
    return true;
  }
  if (consider_overridden_methods) {
    const auto& overridden_methods = mog::get_overridden_methods(
        method_override_graph, method, /* include_interfaces */ true);
    for (auto overridden_method : overridden_methods) {
      if (can_have_unknown_implementations(
              method_override_graph, overridden_method,
              /* consider_overridden_methods */ false)) {
        return true;
      }
    }
  }
  return false;
};

/**
 * Gather candidates of true virtual methods that can be inlined and their
 * call site in true_virtual_callers.
 * A true virtual method can be inlined to its callsite if the callsite can
 * be resolved to only one method implementation deterministically.
 * We are currently ruling out candidates that use the receiver in ways that
 * would require additional casts.
 */
void gather_true_virtual_methods(const mog::Graph& method_override_graph,
                                 const Scope& scope,
                                 CalleeCallerInsns* true_virtual_callers) {
  Timer t("gather_true_virtual_methods");
  auto non_virtual = mog::get_non_true_virtuals(method_override_graph, scope);
  auto same_implementation_map =
      get_same_implementation_map(scope, method_override_graph);
  ConcurrentMap<const DexMethod*, CallerInsns> concurrent_true_virtual_callers;
  ConcurrentMap<IRInstruction*, SameImplementation*>
      same_implementation_invokes;
  // Add mapping from callee to monomorphic callsites.
  auto add_monomorphic_call_site = [&](const DexMethod* caller,
                                       IRInstruction* callsite,
                                       const DexMethod* callee) {
    concurrent_true_virtual_callers.update(
        callee, [&](const DexMethod*, CallerInsns& m, bool) {
          m.caller_insns[caller].emplace(callsite);
        });
  };
  auto add_other_call_site = [&](const DexMethod* callee) {
    concurrent_true_virtual_callers.update(
        callee, [&](const DexMethod*, CallerInsns& m, bool) {
          m.other_call_sites = true;
        });
  };
  auto add_candidate = [&](const DexMethod* callee) {
    concurrent_true_virtual_callers.emplace(callee, CallerInsns());
  };

  walk::parallel::methods(scope, [&non_virtual, &method_override_graph,
                                  &add_monomorphic_call_site,
                                  &add_other_call_site, &add_candidate,
                                  &same_implementation_invokes,
                                  &same_implementation_map](DexMethod* method) {
    if (method->is_virtual() && !non_virtual.count(method)) {
      add_candidate(method);
      if (root(method)) {
        add_other_call_site(method);
      } else {
        const auto& overridden_methods = mog::get_overridden_methods(
            method_override_graph, method, /* include_interfaces */ true);
        for (auto overridden_method : overridden_methods) {
          if (root(overridden_method) || overridden_method->is_external()) {
            add_other_call_site(method);
            break;
          }
        }
      }
    }
    auto code = method->get_code();
    if (!code) {
      return;
    }
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (insn->opcode() != OPCODE_INVOKE_VIRTUAL &&
          insn->opcode() != OPCODE_INVOKE_INTERFACE &&
          insn->opcode() != OPCODE_INVOKE_SUPER) {
        continue;
      }
      auto insn_method = insn->get_method();
      auto callee = resolve_method(insn_method, opcode_to_search(insn), method);
      if (callee == nullptr) {
        // There are some invoke-virtual call on methods whose def are
        // actually in interface.
        callee = resolve_method(insn->get_method(), MethodSearch::Interface);
      }
      if (callee == nullptr) {
        continue;
      }
      if (non_virtual.count(callee) != 0) {
        // Not true virtual, no need to continue;
        continue;
      }
      if (can_have_unknown_implementations(method_override_graph, callee)) {
        add_other_call_site(callee);
        if (insn->opcode() != OPCODE_INVOKE_SUPER) {
          auto overriding_methods =
              mog::get_overriding_methods(method_override_graph, callee);
          for (auto overriding_method : overriding_methods) {
            add_other_call_site(overriding_method);
          }
        }
        continue;
      }
      always_assert_log(callee->is_def(), "Resolved method not def %s",
                        SHOW(callee));
      if (insn->opcode() == OPCODE_INVOKE_SUPER) {
        add_monomorphic_call_site(method, insn, callee);
        continue;
      }
      auto it = same_implementation_map.find(callee);
      if (it != same_implementation_map.end()) {
        // We can find the resolved callee in same_implementation_map,
        // just use that piece of info because we know the implementors are all
        // the same
        add_monomorphic_call_site(method, insn, it->second->representative);
        same_implementation_invokes.emplace(insn, it->second.get());
        continue;
      }
      auto overriding_methods =
          mog::get_overriding_methods(method_override_graph, callee);
      std20::erase_if(overriding_methods,
                      [&](auto* m) { return is_abstract(m); });
      if (overriding_methods.empty()) {
        // There is no override for this method
        add_monomorphic_call_site(method, insn, callee);
      } else if (is_abstract(callee) && overriding_methods.size() == 1) {
        // The method is an abstract method, the only override is its
        // implementation.
        auto implementing_method = *overriding_methods.begin();
        add_monomorphic_call_site(method, insn, implementing_method);
      } else {
        add_other_call_site(callee);
        for (auto overriding_method : overriding_methods) {
          add_other_call_site(overriding_method);
        }
      }
    }
  });

  // Post processing candidates.
  std::vector<const DexMethod*> true_virtual_callees;
  for (auto& p : concurrent_true_virtual_callers) {
    true_virtual_callees.push_back(p.first);
  }
  workqueue_run<const DexMethod*>(
      [&](sparta::SpartaWorkerState<const DexMethod*>*,
          const DexMethod* callee) {
        auto& caller_to_invocations =
            concurrent_true_virtual_callers.at_unsafe(callee);
        if (caller_to_invocations.caller_insns.empty()) {
          return;
        }
        auto code = const_cast<DexMethod*>(callee)->get_code();
        if (!code || !method::no_invoke_super(*code)) {
          if (!caller_to_invocations.caller_insns.empty()) {
            caller_to_invocations.caller_insns.clear();
            caller_to_invocations.other_call_sites = true;
          }
          return;
        }
        // Figure out if candidates use the receiver in a way that does require
        // a cast.
        std::unordered_set<live_range::Use> first_load_param_uses;
        {
          code->build_cfg(/* editable */ true);
          live_range::MoveAwareChains chains(code->cfg());
          auto ii = InstructionIterable(code->cfg().get_param_instructions());
          auto first_load_param = ii.begin()->insn;
          first_load_param_uses =
              std::move(chains.get_def_use_chains()[first_load_param]);
          code->clear_cfg();
        }
        std::unordered_set<DexType*> formal_callee_types;
        bool any_same_implementation_invokes{false};
        for (auto& p : caller_to_invocations.caller_insns) {
          for (auto insn : p.second) {
            formal_callee_types.insert(insn->get_method()->get_class());
            if (same_implementation_invokes.count_unsafe(insn)) {
              any_same_implementation_invokes = true;
            }
          }
        }
        auto type_demands = std::make_unique<std::unordered_set<DexType*>>();
        // Note that the callee-rtype is the same for all methods in a
        // same-implementations cluster.
        auto callee_rtype = callee->get_proto()->get_rtype();
        for (auto use : first_load_param_uses) {
          if (opcode::is_a_move(use.insn->opcode())) {
            continue;
          }
          auto type_demand = get_receiver_type_demand(callee_rtype, use);
          always_assert(type::check_cast(callee->get_class(), type_demand));
          if (type_demand == nullptr) {
            formal_callee_types.clear();
            type_demands = nullptr;
            break;
          }
          always_assert_log(type::check_cast(callee->get_class(), type_demand),
                            "For the incoming code to be type correct, %s must "
                            "be castable to %s.",
                            SHOW(callee->get_class()), SHOW(type_demand));
          if (type_demands->insert(type_demand).second) {
            std20::erase_if(formal_callee_types, [&](auto* t) {
              return !type::check_cast(t, type_demand);
            });
          }
        }
        for (auto& p : caller_to_invocations.caller_insns) {
          for (auto it = p.second.begin(); it != p.second.end();) {
            auto insn = *it;
            if (!formal_callee_types.count(insn->get_method()->get_class())) {
              auto it2 = same_implementation_invokes.find(insn);
              if (it2 != same_implementation_invokes.end()) {
                always_assert(any_same_implementation_invokes);
                auto combined_type_demand = reduce_type_demands(type_demands);
                if (combined_type_demand) {
                  for (auto same_implementation_callee : it2->second->methods) {
                    always_assert_log(
                        type::check_cast(
                            same_implementation_callee->get_class(),
                            combined_type_demand),
                        "For the incoming code to be type correct, %s must "
                        "be castable to %s.",
                        SHOW(same_implementation_callee->get_class()),
                        SHOW(combined_type_demand));
                  }
                  caller_to_invocations.inlined_invokes_need_cast.emplace(
                      insn, combined_type_demand);
                } else {
                  // We can't just cast to the type of the representative. And
                  // it's not trivial to find the right common base type of the
                  // representatives, it might not even exist. (Imagine all
                  // subtypes happen the implement a set of interfaces.)
                  // TODO: Try harder.
                  caller_to_invocations.other_call_sites = true;
                  it = p.second.erase(it);
                  continue;
                }
              } else {
                caller_to_invocations.inlined_invokes_need_cast.emplace(
                    insn, callee->get_class());
              }
            }
            it++;
          }
        }
        std20::erase_if(caller_to_invocations.caller_insns,
                        [&](auto& p) { return p.second.empty(); });
      },
      true_virtual_callees);
  for (auto& pair : concurrent_true_virtual_callers) {
    DexMethod* callee = const_cast<DexMethod*>(pair.first);
    true_virtual_callers->emplace(callee, std::move(pair.second));
  }
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

  auto inliner_config = conf.get_inliner_config();
  const api::AndroidSDK* min_sdk_api{nullptr};
  if (inliner_config.check_min_sdk_refs) {
    int32_t min_sdk = mgr.get_redex_options().min_sdk;
    mgr.incr_metric("min_sdk", min_sdk);
    TRACE(INLINE, 2, "min_sdk: %d", min_sdk);
    auto min_sdk_api_file = conf.get_android_sdk_api_file(min_sdk);
    if (!min_sdk_api_file) {
      mgr.incr_metric("min_sdk_no_file", 1);
      TRACE(INLINE, 2, "Android SDK API %d file cannot be found.", min_sdk);
    } else {
      min_sdk_api = &conf.get_android_sdk_api(min_sdk);
    }
  }

  CalleeCallerInsns true_virtual_callers;
  // Gather all inlinable candidates.
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

  auto candidates =
      gather_non_virtual_methods(scope, method_override_graph.get());

  // The candidates list computed above includes all constructors, regardless of
  // whether it's safe to inline them or not. We'll let the inliner decide
  // what to do with constructors.
  bool analyze_and_prune_inits = true;

  if (inliner_config.virtual_inline && inliner_config.true_virtual_inline) {
    gather_true_virtual_methods(*method_override_graph, scope,
                                &true_virtual_callers);
    for (auto& p : true_virtual_callers) {
      candidates.insert(p.first);
    }
  }
  // keep a map from refs to defs or nullptr if no method was found
  ConcurrentMethodRefCache concurrent_resolved_refs;
  auto concurrent_resolver = [&concurrent_resolved_refs](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolved_refs);
  };

  walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ true);
  });
  inliner_config.shrinker.analyze_constructors =
      inliner_config.shrinker.run_const_prop;

  // inline candidates
  MultiMethodInliner inliner(
      scope, stores, candidates, concurrent_resolver, inliner_config,
      intra_dex ? IntraDex : InterDex, true_virtual_callers, inline_for_speed,
      analyze_and_prune_inits, conf.get_pure_methods(), min_sdk_api);
  inliner.inline_methods(/* need_deconstruct */ false);

  walk::parallel::code(scope,
                       [](DexMethod*, IRCode& code) { code.clear_cfg(); });

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t inlined_init_count = 0;
  for (DexMethod* m : inlined) {
    if (method::is_init(m)) {
      inlined_init_count++;
    }
  }

  size_t deleted = 0;
  if (inliner_config.delete_non_virtuals) {
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
    deleted = delete_methods(scope, inlined, delayed_make_static,
                             concurrent_resolver);
  }

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
  TRACE(INLINE, 3, "api level mismatches %ld",
        (size_t)inliner.get_info().api_level_mismatch);
  TRACE(INLINE, 3, "illegal references %ld",
        (size_t)inliner.get_info().problematic_refs);
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
  mgr.incr_metric("cross_store", inliner.get_info().cross_store);
  mgr.incr_metric("api_level_mismatch", inliner.get_info().api_level_mismatch);
  mgr.incr_metric("problematic_refs", inliner.get_info().problematic_refs);
  mgr.incr_metric("caller_too_large", inliner.get_info().caller_too_large);
  mgr.incr_metric("inlined_init_count", inlined_init_count);
  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("calls_not_inlinable",
                  inliner.get_info().calls_not_inlinable);
  mgr.incr_metric("no_returns", inliner.get_info().no_returns);
  mgr.incr_metric("intermediate_shrinkings",
                  inliner.get_info().intermediate_shrinkings);
  mgr.incr_metric("intermediate_remove_unreachable_blocks",
                  inliner.get_info().intermediate_remove_unreachable_blocks);
  mgr.incr_metric("calls_not_inlined", inliner.get_info().calls_not_inlined);
  mgr.incr_metric("methods_removed", deleted);
  mgr.incr_metric("escaped_virtual", inliner.get_info().escaped_virtual);
  mgr.incr_metric("unresolved_methods", inliner.get_info().unresolved_methods);
  mgr.incr_metric("known_public_methods",
                  inliner.get_info().known_public_methods);
  mgr.incr_metric(
      "constant_invoke_callers_analyzed",
      inliner.get_info()
          .call_site_summary_stats.constant_invoke_callers_analyzed);
  mgr.incr_metric(
      "constant_invoke_callers_unreachable",
      inliner.get_info()
          .call_site_summary_stats.constant_invoke_callers_unreachable);
  mgr.incr_metric(
      "constant_invoke_callers_unreachable_blocks",
      inliner.get_info()
          .call_site_summary_stats.constant_invoke_callers_unreachable_blocks);
  mgr.incr_metric("constant_invoke_callers_critical_path_length",
                  inliner.get_info()
                      .call_site_summary_stats
                      .constant_invoke_callers_critical_path_length);
  mgr.incr_metric("constant_invoke_callees_analyzed",
                  inliner.get_info().constant_invoke_callees_analyzed);
  mgr.incr_metric("constant_invoke_callees_no_return",
                  inliner.get_info().constant_invoke_callees_no_return);
  mgr.incr_metric("constant_invoke_callees_unused_results",
                  inliner.get_info().constant_invoke_callees_unused_results);
  mgr.incr_metric("critical_path_length",
                  inliner.get_info().critical_path_length);
  mgr.incr_metric("methods_shrunk", shrinker.get_methods_shrunk());
  mgr.incr_metric("callers", inliner.get_callers());
  if (intra_dex) {
    mgr.incr_metric("x-dex-callees", inliner.get_x_dex_callees());
  }
  mgr.incr_metric(
      "instructions_eliminated_const_prop",
      shrinker.get_const_prop_stats().branches_removed +
          shrinker.get_const_prop_stats().unreachable_instructions_removed +
          shrinker.get_const_prop_stats().branches_forwarded +
          shrinker.get_const_prop_stats().materialized_consts +
          shrinker.get_const_prop_stats().added_param_const +
          shrinker.get_const_prop_stats().throws +
          shrinker.get_const_prop_stats().null_checks);
  {
    ScopedMetrics sm(mgr);
    auto sm_scope = sm.scope("inliner");
    shrinker.log_metrics(sm);
  }
  mgr.incr_metric("instructions_eliminated_cse",
                  shrinker.get_cse_stats().instructions_eliminated);
  mgr.incr_metric("instructions_eliminated_copy_prop",
                  shrinker.get_copy_prop_stats().moves_eliminated);
  mgr.incr_metric(
      "instructions_eliminated_localdce",
      shrinker.get_local_dce_stats().dead_instruction_count +
          shrinker.get_local_dce_stats().unreachable_instruction_count);
  mgr.incr_metric("instructions_eliminated_unreachable",
                  inliner.get_info().unreachable_insns);
  mgr.incr_metric("instructions_eliminated_dedup_blocks",
                  shrinker.get_dedup_blocks_stats().insns_removed);
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
  Timer::add_timer("Inliner.Inlining.inline_callees",
                   inliner.get_inline_callees_seconds());
  Timer::add_timer("Inliner.Inlining.inline_callees_should_inline",
                   inliner.get_inline_callees_should_inline_seconds());
  Timer::add_timer("Inliner.Inlining.inline_callees_init",
                   inliner.get_inline_callees_init_seconds());
  Timer::add_timer("Inliner.Inlining.inline_inlinables",
                   inliner.get_inline_inlinables_seconds());
  Timer::add_timer("Inliner.Inlining.inline_with_cfg",
                   inliner.get_inline_with_cfg_seconds());
  Timer::add_timer("Inliner.Inlining.call_site_inlined_cost",
                   inliner.get_call_site_inlined_cost_seconds());
  Timer::add_timer("Inliner.Inlining.cannot_inline_sketchy_code",
                   inliner.get_cannot_inline_sketchy_code_timer_seconds());
  Timer::add_timer("Inliner.Shrinking.FastRegAlloc",
                   shrinker.get_fast_reg_alloc_seconds());
}
} // namespace inliner
