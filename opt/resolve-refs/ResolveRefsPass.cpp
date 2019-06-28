/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResolveRefsPass.h"

#include "DexUtil.h"
#include "Resolver.h"
#include "Walkers.h"

namespace {

struct RefStats {
  size_t mref_count = 0;
  size_t fref_count = 0;

  void print(PassManager* mgr) {
    TRACE(RESO, 1, "[ref reso] method ref resolved %d\n", mref_count);
    TRACE(RESO, 1, "[ref reso] field ref resolved %d\n", fref_count);
    mgr->incr_metric("method_refs_resolved", mref_count);
    mgr->incr_metric("field_refs_resolved", fref_count);
  }
};

void resolve_method_refs(IRInstruction* insn,
                         RefStats& stats,
                         bool resolve_to_external) {
  always_assert(insn->has_method());
  auto mref = insn->get_method();
  auto mdef = resolve_method(mref, opcode_to_search(insn));
  if (!mdef || mdef == mref) {
    return;
  }
  if (!resolve_to_external && mdef->is_external()) {
    return;
  }
  // If the existing ref is external already we don't touch it. We are likely
  // going to screw up delicated logic in Android support library or Android x
  // that handles different OS versions.
  if (references_external(mref)) {
    return;
  }
  auto cls = type_class(mdef->get_class());
  // Bail out if the def is non public external
  if (cls && cls->is_external() && !is_public(cls)) {
    return;
  }
  TRACE(RESO, 2, "Resolving %s\n\t=>%s\n", SHOW(mref), SHOW(mdef));
  insn->set_method(mdef);
  stats.mref_count++;
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
  if (real_ref && real_ref != fref) {
    TRACE(RESO, 2, "Resolving %s\n\t=>%s\n", SHOW(fref), SHOW(real_ref));
    insn->set_field(real_ref);
    stats.fref_count++;
    auto cls = type_class(real_ref->get_class());
    always_assert(cls != nullptr);
    if (!is_public(cls)) {
      if (cls->is_external()) return;
      set_public(cls);
    }
  }
}

void resolve_refs(const Scope& scope,
                  RefStats& stats,
                  bool resolve_to_external) {
  walk::opcodes(scope,
                [](DexMethod*) { return true; },
                [&](DexMethod* /* m */, IRInstruction* insn) {
                  switch (insn->opcode()) {
                  case OPCODE_INVOKE_VIRTUAL:
                  case OPCODE_INVOKE_SUPER:
                  case OPCODE_INVOKE_INTERFACE:
                  case OPCODE_INVOKE_STATIC:
                    resolve_method_refs(insn, stats, resolve_to_external);
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
                });
}

} // namespace

void ResolveRefsPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* conf */,
                               PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  RefStats stats;
  resolve_refs(scope, stats, m_resolve_to_external);
  stats.print(&mgr);
}

static ResolveRefsPass s_pass;
