/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveRedundantCheckCasts.h"

#include "CheckCastAnalysis.h"
#include "DexClass.h"
#include "PassManager.h"
#include "Walkers.h"

namespace check_casts {

size_t remove_redundant_check_casts_v2(DexMethod* method) {
  if (!method || !method->get_code()) {
    return 0;
  }

  auto* code = method->get_code();
  code->build_cfg(/* editable */ false);
  impl::CheckCastAnalysis analysis(method);
  auto redundant_check_casts = analysis.collect_redundant_checks_replacement();

  for (const auto& pair : redundant_check_casts) {
    MethodItemEntry* to_replace = pair.first;
    boost::optional<IRInstruction*> replacement_opt = pair.second;
    if (replacement_opt) {
      code->replace_opcode(to_replace->insn, *replacement_opt);
    } else {
      auto it = code->iterator_to(*to_replace);
      code->remove_opcode(it);
    }
  }

  return redundant_check_casts.size();
}

void RemoveRedundantCheckCastsPass::run_pass(DexStoresVector& stores,
                                             ConfigFiles&,
                                             PassManager& mgr) {
  auto scope = build_class_scope(stores);

  size_t num_redundant_check_casts = walk::parallel::reduce_methods<size_t>(
      scope,
      [&](DexMethod* method) -> size_t {
        return remove_redundant_check_casts_v2(method);
      },
      std::plus<size_t>());

  mgr.set_metric("num_redundant_check_casts", num_redundant_check_casts);
}

static RemoveRedundantCheckCastsPass s_pass;

} // namespace check_casts
