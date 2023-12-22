/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "AnnoUtils.h"
#include "ClassHierarchy.h"
#include "ConfigFiles.h"
#include "Deleter.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "ScopedCFG.h"
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
    const auto* arg_types = dex_method->get_proto()->get_args();
    return arg_types->at(src_index);
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

static bool has_bridgelike_access(DexMethod* m) {
  return m->is_virtual() &&
         (is_bridge(m) || (is_synthetic(m) && !method::is_constructor(m)));
}

static void filter_candidates_bridge_synth_only(
    PassManager& mgr,
    Scope& scope,
    std::unordered_set<DexMethod*>& candidates,
    const std::string& prefix) {
  ConcurrentSet<DexMethod*> bridgees;
  ConcurrentSet<DexMethod*> getters;
  ConcurrentSet<DexMethod*> ctors;
  ConcurrentSet<DexMethod*> wrappers;
  walk::parallel::code(scope, [&bridgees, &getters, &ctors,
                               &wrappers](DexMethod* method, IRCode& code) {
    cfg::ScopedCFG cfg(&code);
    std::unordered_set<DexFieldRef*> field_refs;
    std::unordered_set<DexMethod*> invoked_methods;
    size_t other = 0;
    for (auto& mie : InstructionIterable(*cfg)) {
      auto insn = mie.insn;
      auto op = insn->opcode();
      if (insn->has_field()) {
        if (opcode::is_an_iget(op) || opcode::is_an_sget(op)) {
          field_refs.insert(insn->get_field());
          continue;
        }
      } else if (insn->has_method()) {
        if (opcode::is_invoke_static(op) || opcode::is_invoke_direct(op)) {
          auto callee = resolve_method(insn->get_method(),
                                       opcode_to_search(insn), method);
          if (callee) {
            invoked_methods.insert(callee);
            continue;
          }
        }
      }
      if (opcode::is_a_load_param(op) || opcode::is_check_cast(op) ||
          opcode::is_move_result_any(op) || opcode::is_a_move(op) ||
          opcode::is_a_return(op)) {
        continue;
      }
      other++;
    }

    if (field_refs.size() + other == 0 && invoked_methods.size() == 1 &&
        has_bridgelike_access(method)) {
      auto bridgee = (*invoked_methods.begin());
      if (method->get_class() == bridgee->get_class()) {
        bridgees.insert(bridgee);
      }
    }

    // Looks for similar patterns as the legacy SynthPass
    if (is_synthetic(method)) {
      if (is_static(method)) {
        if (invoked_methods.size() + other == 0 && field_refs.size() == 1) {
          // trivial_get_field_wrapper / trivial_get_static_field_wrapper
          getters.insert(method);
        }
      } else if (method::is_init(method)) {
        if (field_refs.size() + other == 0 && invoked_methods.size() == 1) {
          // trivial_ctor_wrapper
          ctors.insert(method);
        }
      }
    }
    if (!method->is_virtual() && !method::is_init(method) &&
        field_refs.size() + other == 0 && invoked_methods.size() == 1) {
      // trivial_method_wrapper
      wrappers.insert(method);
    }
  });
  std20::erase_if(candidates, [&](auto method) {
    return !bridgees.count_unsafe(method) && !wrappers.count_unsafe(method) &&
           !getters.count_unsafe(method) && !ctors.count_unsafe(method);
  });
  mgr.incr_metric(prefix + "bridgees", bridgees.size());
  mgr.incr_metric(prefix + "wrappers", wrappers.size());
  mgr.incr_metric(prefix + "getters", getters.size());
  mgr.incr_metric(prefix + "ctors", ctors.size());
}

// When inlining with making local decisions only, we remove some candidates
// with many relevant invokes, as this reduces the length of the critical path
// and thus speeds up the inlining pass, while not reducing the effectiveness of
// the pass in a meaningful way.
static void filter_candidates_local_only(
    PassManager& mgr,
    Scope& scope,
    uint64_t max_relevant_invokes_when_local_only,
    std::unordered_set<DexMethod*>& candidates) {
  ConcurrentSet<DexMethod*> large_candidates;
  walk::parallel::code(scope, [&large_candidates, &candidates,
                               max_relevant_invokes_when_local_only](
                                  DexMethod* caller, IRCode& code) {
    if (!candidates.count(caller)) {
      return;
    }
    size_t relevant_invokes{0};
    for (auto& mie : InstructionIterable(code.cfg())) {
      if (!opcode::is_an_invoke(mie.insn->opcode())) {
        continue;
      }
      auto callee = resolve_method(mie.insn->get_method(),
                                   opcode_to_search(mie.insn), caller);
      if (candidates.count(callee)) {
        relevant_invokes++;
      }
    }
    if (relevant_invokes > max_relevant_invokes_when_local_only) {
      large_candidates.insert(caller);
    }
  });
  std20::erase_if(candidates,
                  [&](auto method) { return large_candidates.count(method); });
  mgr.incr_metric("large_candidates", large_candidates.size());
}

/**
 * Collect all non virtual methods and make all small methods candidates
 * for inlining.
 */
std::unordered_set<DexMethod*> gather_non_virtual_methods(
    Scope& scope,
    const InsertOnlyConcurrentSet<DexMethod*>* non_virtual,
    const std::unordered_set<DexType*>& no_devirtualize_anno) {
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
  if (non_virtual) {
    non_virt_methods = non_virtual->size();
    for (const auto& vmeth : *non_virtual) {
      auto code = vmeth->get_code();
      if (code == nullptr) {
        non_virtual_no_code++;
        continue;
      }
      if (has_any_annotation(vmeth, no_devirtualize_anno)) {
        continue;
      }
      methods.insert(vmeth);
    }
  }

  TRACE(INLINE, 2, "All methods count: %zu", all_methods);
  TRACE(INLINE, 2, "Direct methods count: %zu", direct_methods);
  TRACE(INLINE, 2, "Virtual methods count: %zu", all_methods - direct_methods);
  TRACE(INLINE, 2, "Direct methods no code: %zu", direct_no_code);
  TRACE(INLINE, 2, "Direct methods with code: %zu",
        direct_methods - direct_no_code);
  TRACE(INLINE, 2, "Constructors with or without code: %zu", init);
  TRACE(INLINE, 2, "Static constructors: %zu", clinit);
  TRACE(INLINE, 2, "Static methods: %zu", static_methods);
  TRACE(INLINE, 2, "Private methods: %zu", private_methods);
  TRACE(INLINE, 2, "Virtual methods non virtual count: %zu", non_virt_methods);
  TRACE(INLINE, 2, "Non virtual no code count: %zu", non_virtual_no_code);
  TRACE(INLINE, 2, "Non virtual no strip count: %zu", non_virt_dont_strip);
  TRACE(INLINE, 2, "Don't strip inlinable methods count: %zu", dont_strip);
  return methods;
}

struct SameImplementation {
  DexMethod* representative;
  std::vector<DexMethod*> methods;
};

using SameImplementationMap =
    std::unordered_map<const DexMethod*, std::shared_ptr<SameImplementation>>;

/**
 * Get a map of method -> implementation method that hold the same
 * implementation as the method would perform at run time.
 * So if a abtract method have multiple implementor but they all have the same
 * implementation, we can have a mapping between the abstract method and
 * one of its implementor.
 */
SameImplementationMap get_same_implementation_map(
    const Scope& scope, const mog::Graph& method_override_graph) {
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
      always_assert(method->get_code()->editable_cfg_built());
      if (same_implementation.representative != nullptr) {
        always_assert(same_implementation.representative->get_code()
                          ->editable_cfg_built());
        if (!method->get_code()->cfg().structural_equals(
                same_implementation.representative->get_code()->cfg())) {
          return false;
        }
      }
      if (same_implementation.representative == nullptr ||
          compare_dexmethods(method, same_implementation.representative)) {
        same_implementation.representative = method;
      }
      same_implementation.methods.push_back(method);
      return true;
    };
    for (auto overriding_method : overriding_methods) {
      if (!method::may_be_invoke_target(overriding_method)) {
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
                                      const DexMethod* method) {
  if (method->is_external()) {
    return true;
  }
  // Why can_rename? To mirror what VirtualRenamer looks at.
  if (is_interface(type_class(method->get_class())) &&
      (root(method) || !can_rename(method))) {
    // We cannot rule out that there are dynamically added classes, possibly
    // even created at runtime via Proxy.newProxyInstance, that override
    // this method. So we assume the worst.
    return true;
  }
  // Also check that for all overridden methods.
  return mog::any_overridden_methods(
      method_override_graph, method,
      [&](auto* overridden_method) {
        if (is_interface(type_class(overridden_method->get_class())) &&
            (root(overridden_method) || !can_rename(overridden_method))) {
          return true;
        }
        return false;
      },
      /* include_interfaces */ true);
};

/**
 * Gather candidates of true virtual methods that can be inlined and their
 * call site in true_virtual_callers.
 * A true virtual method can be inlined to its callsite if the callsite can
 * be resolved to only one method implementation deterministically.
 * We are currently ruling out candidates that use the receiver in ways that
 * would require additional casts.
 */
void gather_true_virtual_methods(
    const mog::Graph& method_override_graph,
    const InsertOnlyConcurrentSet<DexMethod*>& non_virtual,
    const Scope& scope,
    const SameImplementationMap& same_implementation_map,
    CalleeCallerInsns* true_virtual_callers) {
  Timer t("gather_true_virtual_methods");
  ConcurrentMap<const DexMethod*, CallerInsns> concurrent_true_virtual_callers;
  InsertOnlyConcurrentMap<IRInstruction*, SameImplementation*>
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
  auto add_other_call_site =
      [&](const DexMethod* callee,
          bool other_call_sites_overriding_methods_added = false) {
        bool res;
        concurrent_true_virtual_callers.update(
            callee, [&](const DexMethod*, CallerInsns& m, bool) {
              m.other_call_sites = true;
              res = m.other_call_sites_overriding_methods_added;
              if (other_call_sites_overriding_methods_added) {
                m.other_call_sites_overriding_methods_added = true;
              }
            });
        return res;
      };
  auto add_candidate = [&](const DexMethod* callee) {
    concurrent_true_virtual_callers.emplace(callee, CallerInsns());
  };

  struct Key {
    DexMethod* callee;
    DexType* static_base_type;
    bool operator==(const Key& other) const {
      return callee == other.callee &&
             static_base_type == other.static_base_type;
    }
  };
  struct Hash {
    size_t operator()(const Key& key) const {
      size_t hash = 0;
      boost::hash_combine(hash, key.callee);
      boost::hash_combine(hash, key.static_base_type);
      return hash;
    }
  };
  InsertOnlyConcurrentMap<Key, std::vector<const DexMethod*>, Hash>
      concurrent_overriding_methods;
  auto get_overriding_methods =
      [&](DexMethod* callee,
          DexType* static_base_type) -> const std::vector<const DexMethod*>& {
    return *concurrent_overriding_methods
                .get_or_create_and_assert_equal(
                    Key{callee, static_base_type},
                    [&](const Key&) {
                      auto overriding_methods = mog::get_overriding_methods(
                          method_override_graph, callee,
                          /* include_interfaces */ false, static_base_type);
                      std20::erase_if(overriding_methods, [&](auto* m) {
                        return !method::may_be_invoke_target(m);
                      });
                      return overriding_methods;
                    })
                .first;
  };

  walk::parallel::methods(scope, [&non_virtual, &method_override_graph,
                                  &add_monomorphic_call_site,
                                  &add_other_call_site, &add_candidate,
                                  &same_implementation_invokes,
                                  &same_implementation_map,
                                  &get_overriding_methods](DexMethod* method) {
    if (method->is_virtual() && !non_virtual.count_unsafe(method)) {
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
    auto& cfg = code->cfg();
    for (auto* block : cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() != OPCODE_INVOKE_VIRTUAL &&
            insn->opcode() != OPCODE_INVOKE_INTERFACE &&
            insn->opcode() != OPCODE_INVOKE_SUPER) {
          continue;
        }
        auto insn_method = insn->get_method();
        auto callee = resolve_invoke_method(insn, method);
        if (callee == nullptr) {
          continue;
        }
        if (non_virtual.count_unsafe(callee) != 0) {
          // Not true virtual, no need to continue;
          continue;
        }
        auto static_base_type = insn_method->get_class();
        if (can_have_unknown_implementations(method_override_graph, callee)) {
          bool consider_overriding_methods =
              insn->opcode() != OPCODE_INVOKE_SUPER;
          if (!add_other_call_site(callee, consider_overriding_methods) &&
              consider_overriding_methods) {
            const auto& overriding_methods =
                get_overriding_methods(callee, static_base_type);
            for (auto overriding_method : overriding_methods) {
              add_other_call_site(
                  overriding_method,
                  /* other_call_sites_overriding_methods_added */ true);
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
          // just use that piece of info because we know the implementors are
          // all the same
          add_monomorphic_call_site(method, insn, it->second->representative);
          same_implementation_invokes.emplace(insn, it->second.get());
          continue;
        }
        const auto& overriding_methods =
            get_overriding_methods(callee, static_base_type);
        if (overriding_methods.empty()) {
          // There is no override for this method
          add_monomorphic_call_site(method, insn, callee);
        } else if (is_abstract(callee) && overriding_methods.size() == 1) {
          // The method is an abstract method, the only override is its
          // implementation.
          auto implementing_method = *overriding_methods.begin();
          add_monomorphic_call_site(method, insn, implementing_method);
        } else {
          if (!add_other_call_site(
                  callee,
                  /* other_call_sites_overriding_methods_added */ true)) {
            for (auto overriding_method : overriding_methods) {
              add_other_call_site(
                  overriding_method,
                  /* other_call_sites_overriding_methods_added */ true);
            }
          }
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
      [&](sparta::WorkerState<const DexMethod*>*, const DexMethod* callee) {
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
        live_range::Uses first_load_param_uses;
        {
          auto ii = InstructionIterable(code->cfg().get_param_instructions());
          auto first_load_param = ii.begin()->insn;
          live_range::MoveAwareChains chains(code->cfg(),
                                             /* ignore_unreachable */ false,
                                             [first_load_param](auto* insn) {
                                               return insn == first_load_param;
                                             });
          first_load_param_uses =
              std::move(chains.get_def_use_chains()[first_load_param]);
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
void run_inliner(
    DexStoresVector& stores,
    PassManager& mgr,
    ConfigFiles& conf,
    InlinerCostConfig inliner_cost_config /* DEFAULT_COST_CONFIG */,
    bool intra_dex /* false */,
    InlineForSpeed* inline_for_speed /* nullptr */,
    bool inline_bridge_synth_only /* false */,
    bool local_only /* false */) {
  always_assert_log(
      !mgr.init_class_lowering_has_run(),
      "Implementation limitation: The inliner could introduce new "
      "init-class instructions.");
  auto scope = build_class_scope(stores);

  auto inliner_config = conf.get_inliner_config();
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  const api::AndroidSDK* min_sdk_api{nullptr};
  if (inliner_config.check_min_sdk_refs) {
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
  bool cross_dex_penalty = true;
  // Gather all inlinable candidates.
  if (intra_dex) {
    inliner_config.apply_intradex_allowlist();
    cross_dex_penalty = false;
  }

  if (inline_for_speed != nullptr) {
    inliner_config.shrink_other_methods = false;
  }

  if (inline_bridge_synth_only) {
    inliner_config.true_virtual_inline = false;
    inliner_config.virtual_inline = false;
    inliner_config.use_call_site_summaries = false;
    inliner_config.shrink_other_methods = false;
    inliner_config.intermediate_shrinking = false;
    inliner_config.multiple_callers = true;
    inliner_config.delete_non_virtuals = true;
    inliner_config.shrinker = shrinker::ShrinkerConfig();
    inliner_config.shrinker.compute_pure_methods = false;
    inliner_config.shrinker.run_const_prop = true;
    cross_dex_penalty = false;
  }

  if (local_only) {
    inliner_config.true_virtual_inline = false;
    inliner_config.shrink_other_methods = false;
    inliner_config.delete_non_virtuals = true;
  }

  inliner_config.unique_inlined_registers = false;

  std::unique_ptr<const mog::Graph> method_override_graph;
  std::unique_ptr<const InsertOnlyConcurrentSet<DexMethod*>> non_virtual;
  if (inliner_config.virtual_inline) {
    method_override_graph = mog::build_graph(scope);
    non_virtual = std::make_unique<const InsertOnlyConcurrentSet<DexMethod*>>(
        mog::get_non_true_virtuals(*method_override_graph, scope));
  }

  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns(), method_override_graph.get());

  auto candidates = gather_non_virtual_methods(scope, non_virtual.get(),
                                               conf.get_do_not_devirt_anon());

  // The candidates list computed above includes all constructors, regardless of
  // whether it's safe to inline them or not. We'll let the inliner decide
  // what to do with constructors.
  bool analyze_and_prune_inits = true;

  std::unique_ptr<SameImplementationMap> same_implementation_map;
  if (inliner_config.virtual_inline && inliner_config.true_virtual_inline) {
    same_implementation_map = std::make_unique<SameImplementationMap>(
        get_same_implementation_map(scope, *method_override_graph));
  }

  if (inliner_config.virtual_inline && inliner_config.true_virtual_inline) {
    gather_true_virtual_methods(*method_override_graph, *non_virtual, scope,
                                *same_implementation_map,
                                &true_virtual_callers);
    same_implementation_map = nullptr;
  }

  // TODO: The Shrinker will later create another method_override_graph. Use
  // this one instead of re-creating.
  method_override_graph = nullptr;
  non_virtual = nullptr;

  if (inline_bridge_synth_only) {
    filter_candidates_bridge_synth_only(mgr, scope, candidates, "initial_");
  }

  if (local_only) {
    filter_candidates_local_only(
        mgr, scope, inliner_config.max_relevant_invokes_when_local_only,
        candidates);
  }

  inliner_config.shrinker.analyze_constructors =
      inliner_config.shrinker.run_const_prop;

  ConcurrentMethodResolver concurrent_method_resolver;
  // inline candidates
  MultiMethodInliner inliner(
      scope, init_classes_with_side_effects, stores, candidates,
      std::ref(concurrent_method_resolver), inliner_config, min_sdk,
      intra_dex ? IntraDex : InterDex, true_virtual_callers, inline_for_speed,
      analyze_and_prune_inits, conf.get_pure_methods(), min_sdk_api,
      cross_dex_penalty,
      /* configured_finalish_field_names */ {}, local_only,
      inliner_cost_config);
  inliner.inline_methods();

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t inlined_init_count = 0;
  for (DexMethod* m : inlined) {
    if (method::is_init(m)) {
      inlined_init_count++;
    }
  }

  std::vector<DexMethod*> deleted;
  if (inliner_config.delete_non_virtuals) {
    // Do not erase true virtual methods that are inlined because we are only
    // inlining callsites that are monomorphic, for polymorphic callsite we
    // didn't inline, but in run time the callsite may still be resolved to
    // those methods that are inlined. We are relying on RMU to clean up
    // true virtual methods that are not referenced.
    for (const auto& pair : true_virtual_callers) {
      inlined.erase(pair.first);
    }
    deleted =
        delete_methods(scope, inlined, std::ref(concurrent_method_resolver));
  }

  if (inline_bridge_synth_only) {
    std::unordered_set<DexMethod*> deleted_set(deleted.begin(), deleted.end());
    std20::erase_if(candidates,
                    [&](auto method) { return deleted_set.count(method); });
    filter_candidates_bridge_synth_only(mgr, scope, candidates, "remaining_");
  }

  if (!deleted.empty()) {
    // Can't really delete because of possible deob links. At least let's erase
    // any code.
    for (auto* m : deleted) {
      m->release_code();
    }
  }

  TRACE(INLINE, 3, "recursive %zu", inliner.get_info().recursive);
  TRACE(INLINE, 3, "max_call_stack_depth %zu",
        inliner.get_info().max_call_stack_depth);
  TRACE(INLINE, 3, "waited seconds %zu", inliner.get_info().waited_seconds);
  TRACE(INLINE, 3, "blocklisted meths %zu",
        (size_t)inliner.get_info().blocklisted);
  TRACE(INLINE, 3, "virtualizing methods %zu",
        (size_t)inliner.get_info().need_vmethod);
  TRACE(INLINE, 3, "invoke super %zu", (size_t)inliner.get_info().invoke_super);
  TRACE(INLINE, 3, "escaped virtual %zu",
        (size_t)inliner.get_info().escaped_virtual);
  TRACE(INLINE, 3, "known non public virtual %zu",
        (size_t)inliner.get_info().non_pub_virtual);
  TRACE(INLINE, 3, "non public ctor %zu",
        (size_t)inliner.get_info().non_pub_ctor);
  TRACE(INLINE, 3, "unknown field %zu",
        (size_t)inliner.get_info().escaped_field);
  TRACE(INLINE, 3, "non public field %zu",
        (size_t)inliner.get_info().non_pub_field);
  TRACE(INLINE, 3, "throws %zu", (size_t)inliner.get_info().throws);
  TRACE(INLINE, 3, "multiple returns %zu",
        (size_t)inliner.get_info().multi_ret);
  TRACE(INLINE, 3, "references cross stores %zu",
        (size_t)inliner.get_info().cross_store);
  TRACE(INLINE, 3, "api level mismatches %zu",
        (size_t)inliner.get_info().api_level_mismatch);
  TRACE(INLINE, 3, "illegal references %zu",
        (size_t)inliner.get_info().problematic_refs);
  TRACE(INLINE, 3, "not found %zu", (size_t)inliner.get_info().not_found);
  TRACE(INLINE, 3, "caller too large %zu",
        (size_t)inliner.get_info().caller_too_large);
  TRACE(INLINE, 3, "inlined ctors %zu", inlined_init_count);
  TRACE(INLINE, 1, "%zu inlined calls over %zu methods and %zu methods removed",
        (size_t)inliner.get_info().calls_inlined, inlined_count,
        deleted.size());

  const auto& shrinker = inliner.get_shrinker();
  mgr.incr_metric("recursive", inliner.get_info().recursive);
  mgr.incr_metric("max_call_stack_depth",
                  inliner.get_info().max_call_stack_depth);
  mgr.incr_metric("cross_store", inliner.get_info().cross_store);
  mgr.incr_metric("api_level_mismatch", inliner.get_info().api_level_mismatch);
  mgr.incr_metric("problematic_refs", inliner.get_info().problematic_refs);
  mgr.incr_metric("caller_too_large", inliner.get_info().caller_too_large);
  mgr.incr_metric("inlined_init_count", inlined_init_count);
  mgr.incr_metric("init_classes", inliner.get_info().init_classes);
  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("kotlin_lambda_inlined",
                  inliner.get_info().kotlin_lambda_inlined);
  mgr.incr_metric("calls_not_inlinable",
                  inliner.get_info().calls_not_inlinable);
  mgr.incr_metric("no_returns", inliner.get_info().no_returns);
  mgr.incr_metric("intermediate_shrinkings",
                  inliner.get_info().intermediate_shrinkings);
  mgr.incr_metric("intermediate_remove_unreachable_blocks",
                  inliner.get_info().intermediate_remove_unreachable_blocks);
  mgr.incr_metric("calls_not_inlined", inliner.get_info().calls_not_inlined);
  mgr.incr_metric("methods_removed", deleted.size());
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
  mgr.incr_metric("instructions_eliminated_localdce_dead",
                  shrinker.get_local_dce_stats().dead_instruction_count);
  mgr.incr_metric("instructions_eliminated_localdce_unreachable",
                  shrinker.get_local_dce_stats().unreachable_instruction_count);
  mgr.incr_metric("instructions_eliminated_unreachable",
                  inliner.get_info().unreachable_insns);
  mgr.incr_metric("instructions_eliminated_dedup_blocks",
                  shrinker.get_dedup_blocks_stats().insns_removed);
  mgr.incr_metric("blocks_eliminated_by_dedup_blocks",
                  shrinker.get_dedup_blocks_stats().blocks_removed);
  mgr.incr_metric("methods_reg_alloced", shrinker.get_methods_reg_alloced());
  mgr.incr_metric("localdce_init_class_instructions_added",
                  shrinker.get_local_dce_stats().init_class_instructions_added);
  mgr.incr_metric(
      "localdce_init_class_instructions",
      shrinker.get_local_dce_stats().init_classes.init_class_instructions);
  mgr.incr_metric("localdce_init_class_instructions_removed",
                  shrinker.get_local_dce_stats()
                      .init_classes.init_class_instructions_removed);
  mgr.incr_metric("localdce_init_class_instructions_refined",
                  shrinker.get_local_dce_stats()
                      .init_classes.init_class_instructions_refined);

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
}
} // namespace inliner
