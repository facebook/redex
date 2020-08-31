/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResolveRefsPass.h"

#include <boost/algorithm/string.hpp>

#include "ApiLevelChecker.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "TypeInference.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace impl {

struct RefStats {
  size_t num_mref_resolved = 0;
  size_t num_fref_resolved = 0;
  size_t num_invoke_virtual_refined = 0;
  size_t num_invoke_interface_replaced = 0;
  size_t num_invoke_super_removed = 0;

  void print(PassManager* mgr) {
    TRACE(RESO, 1, "[ref reso] method ref resolved %d", num_mref_resolved);
    TRACE(RESO, 1, "[ref reso] field ref resolved %d", num_fref_resolved);
    TRACE(RESO,
          1,
          "[ref reso] invoke-virtual refined %d",
          num_invoke_virtual_refined);
    TRACE(RESO,
          1,
          "[ref reso] invoke-interface replaced %d",
          num_invoke_interface_replaced);
    TRACE(RESO,
          1,
          "[ref reso] invoke-super removed %d",
          num_invoke_super_removed);
    mgr->incr_metric("method_refs_resolved", num_mref_resolved);
    mgr->incr_metric("field_refs_resolved", num_fref_resolved);
    mgr->incr_metric("num_invoke_virtual_refined", num_invoke_virtual_refined);
    mgr->incr_metric("num_invoke_interface_replaced",
                     num_invoke_interface_replaced);
    mgr->incr_metric("num_invoke_super_removed", num_invoke_super_removed);
  }

  RefStats& operator+=(const RefStats& that) {
    num_mref_resolved += that.num_mref_resolved;
    num_fref_resolved += that.num_fref_resolved;
    num_invoke_virtual_refined += that.num_invoke_virtual_refined;
    num_invoke_interface_replaced += that.num_invoke_interface_replaced;
    num_invoke_super_removed += that.num_invoke_super_removed;
    return *this;
  }
};

void resolve_method_refs(const DexMethod* caller,
                         IRInstruction* insn,
                         RefStats& stats) {
  always_assert(insn->has_method());
  auto mref = insn->get_method();
  auto mdef = resolve_method(mref, opcode_to_search(insn), caller);
  if (!mdef || mdef == mref) {
    return;
  }
  // Do not handle external refs in the simple resolve path.
  if (mdef->is_external()) {
    return;
  }
  auto cls = type_class(mdef->get_class());
  // Bail out if the def is non public external
  if (cls && cls->is_external() && !is_public(cls)) {
    return;
  }
  TRACE(RESO, 2, "Resolving %s\n\t=>%s", SHOW(mref), SHOW(mdef));
  insn->set_method(mdef);
  stats.num_mref_resolved++;
  if (cls != nullptr && !is_public(cls)) {
    set_public(cls);
  }
}

void resolve_field_refs(IRInstruction* insn,
                        FieldSearch field_search,
                        RefStats& stats) {
  const auto fref = insn->get_field();
  if (fref->is_def()) {
    return;
  }
  const auto real_ref = resolve_field(fref, field_search);
  if (real_ref && !real_ref->is_external() && real_ref != fref) {
    TRACE(RESO, 2, "Resolving %s\n\t=>%s", SHOW(fref), SHOW(real_ref));
    insn->set_field(real_ref);
    stats.num_fref_resolved++;
    auto cls = type_class(real_ref->get_class());
    always_assert(cls != nullptr);
    if (!is_public(cls)) {
      if (cls->is_external()) return;
      set_public(cls);
    }
  }
}

RefStats resolve_refs(DexMethod* method) {
  RefStats stats;
  if (!method || !method->get_code()) {
    return stats;
  }

  for (auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    switch (insn->opcode()) {
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_STATIC:
      resolve_method_refs(method, insn, stats);
      break;
    case OPCODE_SGET:
    case OPCODE_SGET_WIDE:
    case OPCODE_SGET_OBJECT:
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_SHORT:
    case OPCODE_SPUT:
    case OPCODE_SPUT_WIDE:
    case OPCODE_SPUT_OBJECT:
    case OPCODE_SPUT_BOOLEAN:
    case OPCODE_SPUT_BYTE:
    case OPCODE_SPUT_CHAR:
    case OPCODE_SPUT_SHORT:
      resolve_field_refs(insn, FieldSearch::Static, stats);
      break;
    case OPCODE_IGET:
    case OPCODE_IGET_WIDE:
    case OPCODE_IGET_OBJECT:
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_SHORT:
    case OPCODE_IPUT:
    case OPCODE_IPUT_WIDE:
    case OPCODE_IPUT_OBJECT:
    case OPCODE_IPUT_BOOLEAN:
    case OPCODE_IPUT_BYTE:
    case OPCODE_IPUT_CHAR:
    case OPCODE_IPUT_SHORT:
      resolve_field_refs(insn, FieldSearch::Instance, stats);
      break;
    default:
      break;
    }
  }

  return stats;
}

void try_desuperify(const DexMethod* caller,
                    IRInstruction* insn,
                    RefStats& stats) {
  if (!opcode::is_invoke_super(insn->opcode())) {
    return;
  }
  auto cls = type_class(caller->get_class());
  if (cls == nullptr) {
    return;
  }
  // Skip if the callee is an interface default method (037).
  auto callee_cls = type_class(insn->get_method()->get_class());
  if (!callee_cls || is_interface(callee_cls)) {
    return;
  }
  // resolve_method_ref will start its search in the superclass of :cls.
  auto callee = resolve_method_ref(cls, insn->get_method()->get_name(),
                                   insn->get_method()->get_proto(),
                                   MethodSearch::Virtual);
  // External methods may not always be final across runtime versions
  if (callee == nullptr || callee->is_external() || !is_final(callee)) {
    return;
  }

  TRACE(RESO, 5, "Desuperifying %s because %s is final", SHOW(insn),
        SHOW(callee));
  insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
  stats.num_invoke_super_removed++;
}

bool is_excluded_external(const std::vector<std::string>& excluded_externals,
                          const std::string& name) {
  for (auto& excluded : excluded_externals) {
    if (boost::starts_with(name, excluded)) {
      return true;
    }
  }

  return false;
}

boost::optional<DexMethod*> get_inferred_method_def(
    const std::vector<std::string>& excluded_externals,
    const bool is_support_lib,
    DexMethod* callee,
    const DexType* inferred_type) {

  auto inferred_cls = type_class(inferred_type);
  if (!inferred_cls || is_interface(inferred_cls)) {
    return boost::none;
  }
  auto resolved = resolve_method(inferred_cls, callee->get_name(),
                                 callee->get_proto(), MethodSearch::Virtual);
  if (!resolved || !resolved->is_def()) {
    return boost::none;
  }
  auto resolved_cls = type_class(resolved->get_class());
  bool is_external = resolved_cls && resolved_cls->is_external();
  // 1. If the resolved target is an excluded external, we bail.
  if (is_external && is_excluded_external(excluded_externals, show(resolved))) {
    TRACE(RESO, 4, "Bailed on excluded external%s", SHOW(resolved));
    return boost::none;
  }

  // 2. If the resolved target is external and is referenced in support
  // libraries, we bail.
  if (is_external && api::is_android_sdk_type(resolved->get_class()) &&
      is_support_lib) {
    TRACE(RESO, 4, "Bailed on external in support lib %s", SHOW(resolved));
    return boost::none;
  }

  TRACE(RESO, 2, "Inferred to %s for type %s", SHOW(resolved),
        SHOW(inferred_type));
  return boost::optional<DexMethod*>(const_cast<DexMethod*>(resolved));
}

} // namespace impl

using namespace impl;

RefStats ResolveRefsPass::refine_virtual_callsites(DexMethod* method,
                                                   bool desuperify) {
  RefStats stats;
  if (!method || !method->get_code()) {
    return stats;
  }

  auto* code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg);
  inference.run(method);
  auto& envs = inference.get_type_environments();
  auto is_support_lib = api::is_support_lib_type(method->get_class());

  for (auto& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    if (desuperify) {
      try_desuperify(method, insn, stats);
    }

    auto opcode = insn->opcode();
    if (!opcode::is_invoke_virtual(opcode) &&
        !opcode::is_invoke_interface(opcode)) {
      continue;
    }

    auto mref = insn->get_method();
    auto callee = resolve_method(mref, opcode_to_search(insn), method);
    if (!callee) {
      continue;
    }

    auto this_reg = insn->src(0);
    auto& env = envs.at(insn);
    auto dex_type = env.get_dex_type(this_reg);

    if (!dex_type || callee->get_class() == *dex_type) {
      // Unsuccessful inference or inferred to the same type.
      continue;
    }

    // replace it with the actual implementation if any provided.
    auto m_def = get_inferred_method_def(m_excluded_externals, is_support_lib,
                                         callee, *dex_type);
    if (!m_def) {
      continue;
    }
    auto def_meth = *m_def;
    auto def_cls = type_class((def_meth)->get_class());
    if (!def_cls) {
      continue;
    }
    if (mref == def_meth) {
      continue;
    }
    // Stop if the resolve_to_external config is False.
    if (!m_resolve_to_external && def_cls->is_external()) {
      TRACE(RESO, 4, "Bailed on external %s", SHOW(def_meth));
      continue;
    } else if (def_cls->is_external() && !m_min_sdk_api->has_method(def_meth)) {
      // Resolving to external and the target is missing in the min_sdk_api.
      TRACE(RESO, 4, "Bailed on mismatch with min_sdk %s", SHOW(def_meth));
      continue;
    }
    TRACE(RESO, 2, "Resolving %s\n\t=>%s", SHOW(mref), SHOW(def_meth));
    insn->set_method(def_meth);
    if (opcode::is_invoke_interface(opcode) && !is_interface(def_cls)) {
      insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
      stats.num_invoke_interface_replaced++;
    } else {
      stats.num_invoke_virtual_refined++;
    }
  }

  return stats;
}

void ResolveRefsPass::eval_pass(DexStoresVector&,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  // Disable resolution to external for API level older than 19.
  if (m_resolve_to_external &&
      min_sdk < m_supported_min_sdk_for_external_refs) {
    m_resolve_to_external = false;
    TRACE(RESO, 2, "Disabling resolution to external for min_sdk %d", min_sdk);
  }

  // Load min_sdk API file
  auto min_sdk_api_file = conf.get_android_sdk_api_file(min_sdk);
  if (!min_sdk_api_file) {
    TRACE(RESO, 2, "Android SDK API %d file cannot be found.", min_sdk);
    always_assert_log(
        !m_resolve_to_external ||
            min_sdk < m_supported_min_sdk_for_external_refs,
        "Android SDK API %d file can not be found but resolve_to_external is "
        "explicitly enabled for this version. Please pass the api list to "
        "Redex or turn off `resolve_to_external`.",
        min_sdk);
    m_resolve_to_external = false;
  } else {
    TRACE(RESO, 2, "Android SDK API %d file found: %s", min_sdk,
          min_sdk_api_file->c_str());
  }

  m_min_sdk_api = &conf.get_android_sdk_api(min_sdk);
}

void ResolveRefsPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* conf */,
                               PassManager& mgr) {
  always_assert(m_min_sdk_api);
  Scope scope = build_class_scope(stores);
  impl::RefStats stats =
      walk::parallel::methods<impl::RefStats>(scope, [&](DexMethod* method) {
        auto local_stats = impl::resolve_refs(method);
        local_stats += refine_virtual_callsites(method, m_desuperify);
        return local_stats;
      });
  stats.print(&mgr);
}

static ResolveRefsPass s_pass;
