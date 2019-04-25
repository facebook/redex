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
#include "ReachableClasses.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

namespace {
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
      dont_inline = true;
    } else {
      (is_static(method)) ? static_methods++ : private_methods++;
    }

    if (dont_inline) return;

    methods.insert(method);
  });
  if (virtual_inline) {
    auto non_virtual = devirtualize(scope);
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

  TRACE(INLINE, 2, "All methods count: %ld\n", all_methods);
  TRACE(INLINE, 2, "Direct methods count: %ld\n", direct_methods);
  TRACE(INLINE, 2, "Virtual methods count: %ld\n",
        all_methods - direct_methods);
  TRACE(INLINE, 2, "Direct methods no code: %ld\n", direct_no_code);
  TRACE(INLINE, 2, "Direct methods with code: %ld\n",
        direct_methods - direct_no_code);
  TRACE(INLINE, 2, "Constructors with or without code: %ld\n", init);
  TRACE(INLINE, 2, "Static constructors: %ld\n", clinit);
  TRACE(INLINE, 2, "Static methods: %ld\n", static_methods);
  TRACE(INLINE, 2, "Private methods: %ld\n", private_methods);
  TRACE(INLINE, 2, "Virtual methods non virtual count: %ld\n",
        non_virt_methods);
  TRACE(INLINE, 2, "Non virtual no code count: %ld\n", non_virtual_no_code);
  TRACE(INLINE, 2, "Non virtual no strip count: %ld\n", non_virt_dont_strip);
  TRACE(INLINE, 2, "Don't strip inlinable methods count: %ld\n", dont_strip);
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
                             intra_dex);
  inliner.inline_methods();

  if (inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope,
                         [](DexMethod*, IRCode& code) { code.clear_cfg(); });
  }

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t deleted = delete_methods(scope, inlined, resolver);

  TRACE(INLINE, 3, "recursive %ld\n", inliner.get_info().recursive);
  TRACE(INLINE, 3, "blacklisted meths %ld\n", inliner.get_info().blacklisted);
  TRACE(INLINE, 3, "virtualizing methods %ld\n",
        inliner.get_info().need_vmethod);
  TRACE(INLINE, 3, "invoke super %ld\n", inliner.get_info().invoke_super);
  TRACE(INLINE, 3, "override inputs %ld\n", inliner.get_info().write_over_ins);
  TRACE(INLINE, 3, "escaped virtual %ld\n", inliner.get_info().escaped_virtual);
  TRACE(INLINE, 3, "known non public virtual %ld\n",
        inliner.get_info().non_pub_virtual);
  TRACE(INLINE, 3, "non public ctor %ld\n", inliner.get_info().non_pub_ctor);
  TRACE(INLINE, 3, "unknown field %ld\n", inliner.get_info().escaped_field);
  TRACE(INLINE, 3, "non public field %ld\n", inliner.get_info().non_pub_field);
  TRACE(INLINE, 3, "throws %ld\n", inliner.get_info().throws);
  TRACE(INLINE, 3, "multiple returns %ld\n", inliner.get_info().multi_ret);
  TRACE(INLINE, 3, "references cross stores %ld\n",
        inliner.get_info().cross_store);
  TRACE(INLINE, 3, "not found %ld\n", inliner.get_info().not_found);
  TRACE(INLINE, 3, "caller too large %ld\n",
        inliner.get_info().caller_too_large);
  TRACE(INLINE, 1,
        "%ld inlined calls over %ld methods and %ld methods removed\n",
        inliner.get_info().calls_inlined, inlined_count, deleted);

  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("methods_removed", deleted);
  mgr.incr_metric("escaped_virtual", inliner.get_info().escaped_virtual);
  mgr.incr_metric("unresolved_methods", inliner.get_info().unresolved_methods);
  mgr.incr_metric("method_oks", inliner.get_info().method_oks);
}
} // namespace inliner
