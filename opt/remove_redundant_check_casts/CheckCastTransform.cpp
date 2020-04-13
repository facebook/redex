/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CheckCastTransform.h"

#include "DexUtil.h"
#include "ReachingDefinitions.h"
#include "TypeInference.h"

namespace check_casts {

namespace impl {

Stats apply(DexMethod* method, const CheckCastReplacements& casts) {
  Stats stats;
  auto& cfg = method->get_code()->cfg();
  for (const auto& cast : casts) {
    auto it = cfg.find_insn(cast.insn, cast.block);
    boost::optional<IRInstruction*> replacement_insn = cast.replacement_insn;
    if (replacement_insn) {
      cfg.replace_insn(it, *replacement_insn);
      stats.replaced_casts++;
    } else {
      boost::optional<DexType*> replacement_type = cast.replacement_type;
      if (replacement_type) {
        cast.insn->set_type(*replacement_type);
        stats.weakened_casts++;
      } else {
        cfg.remove_insn(it);
        stats.removed_casts++;
      }
    }
  }

  return stats;
}

Stats& Stats::operator+=(const Stats& that) {
  removed_casts += that.removed_casts;
  replaced_casts += that.replaced_casts;
  weakened_casts += that.weakened_casts;
  return *this;
}

} // namespace impl

} // namespace check_casts
