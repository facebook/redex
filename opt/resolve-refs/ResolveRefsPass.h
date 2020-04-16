/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include "ApiLevelsUtils.h"

/**
 * A field or method being referenced by an instruction could be a pure `ref`.
 * In which, the ref points to a class where the field/method is not actually
 * defined. This is allowed in dex bytecode. However, it adds complexity to
 * Redex's optimizations.
 *
 * The motivation of this pass is to resolve all
 * method/field references to its definition in the most accurate way possible.
 * It is supposed to be done early on, so that the rest of the optimizations
 * don't have to deal with the distinction between a `ref` and a `def`.
 *
 * Unlike RebindRefs, the goal here is to bind the method/field reference to the
 * most accurate one possible to produce an accurate reachability graph of the
 * program. Therefore, the number of unique method references is not a concern.
 */

namespace impl {
struct RefStats;
} // namespace impl

class ResolveRefsPass : public Pass {
 public:
  ResolveRefsPass() : Pass("ResolveRefsPass") {}

  void bind_config() override {
    bind("resolve_to_external", false, m_resolve_to_external,
         "Allowing resolving method ref to an external one");
    bind("desuperify", true, m_desuperify,
         "Convert invoke-super calls to invoke-virtual where possible");
    bind("excluded_externals", {}, m_excluded_externals,
         "Externals types/prefixes excluded from reference resolution");
  }

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  impl::RefStats refine_virtual_callsites(DexMethod* method, bool desuperify);

  bool m_resolve_to_external;
  bool m_desuperify;
  std::vector<std::string> m_excluded_externals;
  const api::AndroidSDK* m_min_sdk_api;
};
