/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Deleter.h"
#include "InlineInit.h"
#include "Inliner.h"
#include "Resolver.h"
#include "Walkers.h"

/*
 * Returns
 *   {<init> methods} ∩ {methods with < 3 opcodes}
 *   ∪ ({methods that are deletable} ∩ {methods with a single callsite})
 *   - {methods that are called from the primary dex}
 */
std::unordered_set<DexMethod*> InlineInitPass::gather_init_candidates(
    const Scope& scope, const DexClasses& primary_dex) {
  constexpr int SMALL_CODE_SIZE = 3;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> deletable_ctors;
  walk::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (is_constructor(method) && !is_static(method)) {
      if (code->count_opcodes() < SMALL_CODE_SIZE) {
        candidates.emplace(method);
      } else if (!can_delete(method)) {
        deletable_ctors.insert(method);
      }
    }
  });
  select_inlinable(scope, deletable_ctors, m_resolved_refs, &candidates);

  return candidates;
}

static void make_ifields_non_final(DexClass* cls) {
  for (auto ifield : cls->get_ifields()) {
    ifield->set_access(ifield->get_access() & ~ACC_FINAL);
  }
}

void InlineInitPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& cfg,
                              PassManager& mgr) {

  if (!mgr.verify_none_enabled()) {
    TRACE(INLINIT, 1, "Verify-none mode is disabled, skipping Inline Init...\n");
    return;
  }

  auto scope = build_class_scope(stores);
  auto& primary_dex = stores[0].get_dexen()[0];

  auto resolver = [&](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, m_resolved_refs);
  };

  auto inlinable = gather_init_candidates(scope, primary_dex);

  for (auto cls : primary_dex) {
    m_inliner_config.caller_black_list.emplace(cls->get_type());
  }

  MultiMethodInliner inliner(
      scope, stores, inlinable, resolver, m_inliner_config);
  inliner.inline_methods();

  auto inlined = inliner.get_inlined();

  for (DexMethod* m : inlined) {
    make_ifields_non_final(type_class(m->get_class()));
  }

  size_t deleted = delete_methods(scope, inlined, resolver);

  mgr.incr_metric("candidates", inlinable.size());
  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("methods_removed", deleted);
}

static InlineInitPass s_pass;
