/**
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
#include "Deleter.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Inliner.h"
#include "MethodOverrideGraph.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

static bool can_inline_init(DexMethod* caller, IRCode& code) {
  always_assert(is_init(caller));
  // Check that there is no call to a super constructor, and no assignments to
  // (non-inherited) instance fields before constructor call.
  // (There not being such a super call implies that there must be a call to
  // another constructor in the same class, unless the method doesn't return;
  // calls to other constructors in the same class are inlinable.)
  // The check doesn't take into account data-flow, i.e. whether the super
  // constructor call and the field assignments are actually on the incoming
  // receiver object. In that sense, this function is overly conservative, and
  // there is room for futher improvement.
  DexType* declaring_type = caller->get_class();
  DexType* super_type = type_class(declaring_type)->get_super_class();
  for (auto& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    auto opcode = insn->opcode();

    // give up if there's an assignment to a field of the declaring class
    if (is_iput(opcode) && insn->get_field()->get_class() == declaring_type) {
      return false;
    }

    // give up if there's a call to a constructor of the super class
    if (opcode != OPCODE_INVOKE_DIRECT) {
      continue;
    }
    DexMethod* callee =
        resolve_method(insn->get_method(), MethodSearch::Direct);
    if (callee == nullptr) {
      return false;
    }
    if (!is_init(callee)) {
      continue;
    }
    if (callee->get_class() == super_type) {
      return false;
    }
  }
  return true;
}

/**
 * Collect all non virtual methods and make all small methods candidates
 * for inlining.
 */
std::unordered_set<DexMethod*> gather_non_virtual_methods(Scope& scope,
                                                          bool virtual_inline) {
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
    if (is_constructor(method)) {
      (is_static(method)) ? clinit++ : init++;
      if (is_clinit(method) || !can_inline_init(method, *code)) {
        dont_inline = true;
      }
    } else {
      (is_static(method)) ? static_methods++ : private_methods++;
    }

    if (dont_inline) return;

    methods.insert(method);
  });
  if (virtual_inline) {
    auto non_virtual = mog::get_non_true_virtuals(scope);
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
} // namespace

namespace inliner {
void run_inliner(DexStoresVector& stores,
                 PassManager& mgr,
                 const InlinerConfig& inliner_config,
                 bool intra_dex /*false*/) {
  if (mgr.no_proguard_rules()) {
    TRACE(INLINE, 1,
          "MethodInlinePass not run because no ProGuard configuration was "
          "provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  // Gather all inlinable candidates.
  auto methods =
      gather_non_virtual_methods(scope, inliner_config.virtual_inline);

  // keep a map from refs to defs or nullptr if no method was found
  MethodRefCache resolved_refs;
  auto resolver = [&resolved_refs](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, resolved_refs);
  };
  if (inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
      code.build_cfg(/* editable */ true);
    });
  }

  // inline candidates
  MultiMethodInliner inliner(scope, stores, methods, resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();

  if (inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope,
                         [](DexMethod*, IRCode& code) { code.clear_cfg(); });
  }

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t deleted = delete_methods(scope, inlined, resolver);

  TRACE(INLINE, 3, "recursive %ld", inliner.get_info().recursive);
  TRACE(INLINE, 3, "blacklisted meths %ld", inliner.get_info().blacklisted);
  TRACE(INLINE, 3, "virtualizing methods %ld",
        inliner.get_info().need_vmethod);
  TRACE(INLINE, 3, "invoke super %ld", inliner.get_info().invoke_super);
  TRACE(INLINE, 3, "override inputs %ld", inliner.get_info().write_over_ins);
  TRACE(INLINE, 3, "escaped virtual %ld", inliner.get_info().escaped_virtual);
  TRACE(INLINE, 3, "known non public virtual %ld",
        inliner.get_info().non_pub_virtual);
  TRACE(INLINE, 3, "non public ctor %ld", inliner.get_info().non_pub_ctor);
  TRACE(INLINE, 3, "unknown field %ld", inliner.get_info().escaped_field);
  TRACE(INLINE, 3, "non public field %ld", inliner.get_info().non_pub_field);
  TRACE(INLINE, 3, "throws %ld", inliner.get_info().throws);
  TRACE(INLINE, 3, "multiple returns %ld", inliner.get_info().multi_ret);
  TRACE(INLINE, 3, "references cross stores %ld",
        inliner.get_info().cross_store);
  TRACE(INLINE, 3, "not found %ld", inliner.get_info().not_found);
  TRACE(INLINE, 3, "caller too large %ld",
        inliner.get_info().caller_too_large);
  TRACE(INLINE, 1,
        "%ld inlined calls over %ld methods and %ld methods removed",
        inliner.get_info().calls_inlined, inlined_count, deleted);

  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("methods_removed", deleted);
  mgr.incr_metric("escaped_virtual", inliner.get_info().escaped_virtual);
  mgr.incr_metric("unresolved_methods", inliner.get_info().unresolved_methods);
  mgr.incr_metric("known_public_methods",
                  inliner.get_info().known_public_methods);
}
} // namespace inliner
