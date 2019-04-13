/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Bridge.h"

#include <stdio.h>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/functional/hash.hpp>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "Inliner.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "Trace.h"
#include "ClassHierarchy.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_BRIDGES_REMOVED = "bridges_removed_count";
constexpr const char* METRIC_BRIDGES_TO_OPTIMIZE = "bridges_to_optimize_count";

DexMethodRef* match_pattern(DexMethod* bridge) {
  auto code = bridge->get_code();
  if (!code) return nullptr;
  auto ii = InstructionIterable(code);
  auto it = ii.begin();
  auto end = ii.end();
  while (it != end && opcode::is_load_param(it->insn->opcode())) {
    ++it;
  }
  while (it != end) {
    if (it->insn->opcode() != OPCODE_CHECK_CAST) break;
    // skip past the move-result-pseudo opcode
    std::advance(it, 2);
  }
  always_assert_log(it != end, "In %s", SHOW(bridge));
  if (it->insn->opcode() != OPCODE_INVOKE_DIRECT &&
      it->insn->opcode() != OPCODE_INVOKE_STATIC) {
    TRACE(BRIDGE, 5, "Rejecting unhandled pattern: `%s'\n", SHOW(bridge));
    return nullptr;
  }
  auto invoke = it->insn;
  ++it;

  if (is_move_result(it->insn->opcode())) {
    ++it;
  }
  if (!is_return(it->insn->opcode())) {
    TRACE(BRIDGE, 5, "Rejecting unhandled pattern: `%s'\n", SHOW(bridge));
    return nullptr;
  }
  ++it;
  if (it != end) return nullptr;
  return invoke->get_method();
}

bool is_optimization_candidate(DexMethod* bridge, DexMethod* bridgee) {
  if (!can_delete(bridgee)) {
    TRACE(BRIDGE, 5,
          "Cannot delete bridgee! bridge: %s\n bridgee: %s\n",
          SHOW(bridge),
          SHOW(bridgee));
    return false;
  }
  if (!bridgee->get_code()) {
    TRACE(BRIDGE, 5, "Rejecting, bridgee has no code: `%s'\n", SHOW(bridge));
    return false;
  }
  return true;
}

DexMethod* find_bridgee(DexMethod* bridge) {
  auto bridgee_ref = match_pattern(bridge);
  if (!bridgee_ref) return nullptr;
  if (!bridgee_ref->is_def()) return nullptr;
  auto bridgee = static_cast<DexMethod*>(bridgee_ref);
  if (!is_optimization_candidate(bridge, bridgee)) return nullptr;
  return bridgee;
}

bool signature_matches(DexMethod* a, DexMethod* b) {
  return a->get_name() == b->get_name() && a->get_proto() == b->get_proto();
}

bool has_bridgelike_access(DexMethod* m) {
  return
    m->is_virtual() &&
    (is_bridge(m) ||
     (is_synthetic(m) && !is_static(m) && !is_constructor(m)));
}

void do_inlining(DexMethod* bridge, DexMethod* bridgee) {
  bridge->set_access(bridge->get_access() & ~(ACC_BRIDGE | ACC_SYNTHETIC));
  auto code = bridge->get_code();
  auto invoke =
      std::find_if(code->begin(), code->end(), [](const MethodItemEntry& mie) {
        return mie.type == MFLOW_OPCODE && is_invoke(mie.insn->opcode());
      });
  inliner::inline_tail_call(bridge, bridgee, invoke);
}
}

////////////////////////////////////////////////////////////////////////////////

/*
 * Synthetic bridge removal optimization.
 *
 * This pass removes bridge methods that javac creates to provide argument and
 * return-type covariance.  Bridge methods take the general form:
 *
 *     check-cast*   (for checking covariant arg types)
 *     invoke-{direct,virtual,static}  bridged-method
 *     move-result
 *     return
 *
 * For conciseness we refer to the bridged method as the "bridgee".  To
 * optimize this pattern we inline the bridgee into the bridge, by replacing
 * the invoke- and adjusting the check-casts as necessary.  We can then delete
 * the bridgee.
 *
 * If the bridgee is referenced directly by any method other than the bridge,
 * we don't apply this optimization.  In this case we couldn't safely remove
 * the bridgee, so inlining it somewhere would simply bloat the code.
 *
 * NB: The BRIDGE access flag isn't used for synthetic wrappers that implement
 * args/return of generics, but it's the same concept.
 */
class BridgeRemover {
  using MethodRef = std::tuple<const DexType*, DexString*, DexProto*>;

  struct MethodRefHash {
    size_t operator()(const MethodRef& m) const {
      size_t seed = 0;
      boost::hash_combine(seed, std::get<0>(m));
      boost::hash_combine(seed, std::get<1>(m));
      boost::hash_combine(seed, std::get<2>(m));
      return seed;
    }
  };

  const std::vector<DexClass*>* m_scope;
  ClassHierarchy m_ch;
  PassManager& m_mgr;
  std::unordered_map<DexMethod*, DexMethod*> m_bridges_to_bridgees;
  std::unordered_multimap<MethodRef, DexMethod*, MethodRefHash>
      m_potential_bridgee_refs;

  void find_bridges() {
    walk::methods(*m_scope,
                 [&](DexMethod* m) {
                   if (has_bridgelike_access(m)) {
                     auto bridgee = find_bridgee(m);
                     if (!bridgee) return;
                     m_bridges_to_bridgees.emplace(m, bridgee);
                     TRACE(BRIDGE,
                           5,
                           "Bridge:%p:%s\nBridgee:%p:%s\n",
                           m,
                           SHOW(m),
                           bridgee,
                           SHOW(bridgee));
                   }
                 });
  }

  void search_hierarchy_for_matches(DexMethod* bridge, DexMethod* bridgee) {
    /*
     * Direct reference.  The only one if it's non-virtual.
     */
    auto clstype = bridgee->get_class();
    auto name = bridgee->get_name();
    auto proto = bridgee->get_proto();
    TRACE(BRIDGE, 5, "   %s %s %s\n", SHOW(clstype), SHOW(name), SHOW(proto));
    m_potential_bridgee_refs.emplace(MethodRef(clstype, name, proto), bridge);
    if (!bridgee->is_virtual()) return;

    /*
     * Search super classes
     *
     *   A bridge method in a derived class may be referred to using the name
     *   of a super class if a method with a matching signature is defined in
     *   that super class.
     *
     *   To build the set of potential matches, we accumulate potential refs in
     *   maybe_refs, and when we find a matching signature in a super class, we
     *   add everything in maybe_refs to the set.
     */
    std::vector<std::pair<MethodRef, DexMethod*>> maybe_refs;
    for (auto super = type_class(type_class(clstype)->get_super_class());
         super != nullptr;
         super = type_class(super->get_super_class())) {
      maybe_refs.emplace_back(
          MethodRef(super->get_type(), name, proto), bridge);
      for (auto vmethod : const_cast<const DexClass*>(super)->get_vmethods()) {
        if (signature_matches(bridgee, vmethod)) {
          for (auto DEBUG_ONLY refp : maybe_refs) {
            TRACE(BRIDGE,
                  5,
                  "    %s %s %s\n",
                  SHOW(std::get<0>(refp.first)),
                  SHOW(std::get<1>(refp.first)),
                  SHOW(std::get<2>(refp.first)));
          }
          m_potential_bridgee_refs.insert(maybe_refs.begin(), maybe_refs.end());
          maybe_refs.clear();
        }
      }
    }

    /*
     * Search sub classes
     *
     *   Easy.  Any subclass can refer to the bridgee.
     */
    TypeSet subclasses;
    get_all_children(m_ch, clstype, subclasses);
    for (auto subclass : subclasses) {
      m_potential_bridgee_refs.emplace(MethodRef(subclass, name, proto),
                                       bridge);
      TRACE(BRIDGE,
            5,
            "    %s %s %s\n",
            SHOW(subclass),
            SHOW(name),
            SHOW(proto));
    }
  }

  void find_potential_bridgee_refs() {
    for (auto bpair : m_bridges_to_bridgees) {
      auto bridge = bpair.first;
      auto bridgee = bpair.second;
      TRACE(BRIDGE, 5, "Bridge method: %s\n", SHOW(bridge));
      TRACE(BRIDGE, 5, "  Bridgee: %s\n", SHOW(bridgee));
      TRACE(BRIDGE, 5, "  Potential references:\n");
      search_hierarchy_for_matches(bridge, bridgee);
    }
  }

  void exclude_referenced_bridgee(DexMethod* code_method, IRCode& code) {
    for (auto& mie : InstructionIterable(&code)) {
      auto inst = mie.insn;
      if (!is_invoke(inst->opcode())) continue;
      auto method = inst->get_method();
      auto range = m_potential_bridgee_refs.equal_range(
          MethodRef(method->get_class(), method->get_name(),
              method->get_proto()));
      for (auto it = range.first; it != range.second; ++it) {
        auto referenced_bridge = it->second;
        // Don't count the bridge itself
        if (referenced_bridge == code_method) continue;
        TRACE(BRIDGE,
              5,
              "Rejecting, reference `%s.%s.%s' in `%s' blocks `%s'\n",
              SHOW(method->get_class()),
              SHOW(method->get_name()),
              SHOW(method->get_proto()),
              SHOW(code_method),
              SHOW(referenced_bridge));
        m_bridges_to_bridgees.erase(referenced_bridge);
      }
    }
  }

  void exclude_referenced_bridgees() {
    std::vector<DexMethodRef*> refs;

    auto visit_methods = [&refs](DexMethod* m) {
        auto const& anno = m->get_anno_set();
        if (anno) anno->gather_methods(refs);
        auto const& param_anno = m->get_param_anno();
        if (param_anno) {
          for (auto const& pair : *param_anno) {
            pair.second->gather_methods(refs);
          }
        }
    };

    for (auto const& cls : *m_scope) {
      auto const& anno = cls->get_anno_set();
      if (anno) anno->gather_methods(refs);
      for (auto const& m : cls->get_dmethods()) {
        visit_methods(m);
      }
      for (auto const& m : cls->get_vmethods()) {
        visit_methods(m);
      }
      for (auto const& f : cls->get_sfields()) {
        f->gather_methods(refs);
      }
      for (auto const& f : cls->get_ifields()) {
        f->gather_methods(refs);
      }
    }

    std::unordered_set<DexMethod*> refs_set;
    for (const auto& ref : refs) {
      if (!ref->is_def()) continue;
      refs_set.insert(static_cast<DexMethod*>(ref));
    }
    std::vector<DexMethod*> kill_me;
    for (auto const& p : m_bridges_to_bridgees) {
      if (refs_set.count(p.second)) {
        kill_me.push_back(p.first);
      }
    }
    for (auto const& kill : kill_me) {
      m_bridges_to_bridgees.erase(kill);
    }

    walk::code(*m_scope,
              [](DexMethod*) { return true; },
              [&](DexMethod* m, IRCode& code) {
                exclude_referenced_bridgee(m, code);
              });
  }

  void inline_bridges() {
    for (auto bpair : m_bridges_to_bridgees) {
      auto bridge = bpair.first;
      auto bridgee = bpair.second;
      TRACE(BRIDGE, 5, "Inlining %s\n", SHOW(bridge));
      do_inlining(bridge, bridgee);
    }
  }

  void delete_unused_bridgees() {
    for (auto bpair : m_bridges_to_bridgees) {
      auto bridge = bpair.first;
      auto bridgee = bpair.second;
      always_assert_log(bridge->is_virtual(),
                        "bridge: %s\nbridgee: %s",
                        SHOW(bridge),
                        SHOW(bridgee));
      // TODO: Bridgee won't necessarily be direct once we expand this
      // optimization
      redex_assert(!bridgee->is_virtual());
      auto cls = type_class(bridgee->get_class());
      cls->remove_method(bridgee);
      DexMethod::erase_method(bridgee);
    }
  }

 public:
  BridgeRemover(const std::vector<DexClass*>& scope, PassManager& mgr)
      : m_scope(&scope), m_mgr(mgr) {
    m_ch = build_type_hierarchy(scope);
  }

  void run() {
    find_bridges();
    find_potential_bridgee_refs();
    exclude_referenced_bridgees();
    TRACE(BRIDGE, 5, "%lu bridges to optimize\n", m_bridges_to_bridgees.size());
    m_mgr.incr_metric(METRIC_BRIDGES_TO_OPTIMIZE, m_bridges_to_bridgees.size());
    inline_bridges();
    delete_unused_bridgees();
    TRACE(BRIDGE, 1,
            "Inlined and removed %lu bridges\n",
            m_bridges_to_bridgees.size());
    m_mgr.incr_metric(METRIC_BRIDGES_REMOVED, m_bridges_to_bridgees.size());
  }
};

////////////////////////////////////////////////////////////////////////////////

void BridgePass::run_pass(DexStoresVector& stores,
                          ConfigFiles& /* conf */,
                          PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(BRIDGE, 1, "BridgePass not run because no ProGuard configuration was provided.");
    return;
  }
  Scope scope = build_class_scope(stores);
  BridgeRemover(scope, mgr).run();
}

static BridgePass s_pass;
