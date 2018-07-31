/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FieldOpTracker.h"

#include "Resolver.h"
#include "Walkers.h"

namespace field_op_tracker {

bool is_own_init(DexField* field, const DexMethod* method) {
  return (is_clinit(method) || is_init(method)) &&
         method->get_class() == field->get_class();
}

FieldStatsMap analyze(const Scope& scope) {
  FieldStatsMap field_stats;
  // Gather the read/write counts.
  walk::opcodes(scope, [&](const DexMethod* method, const IRInstruction* insn) {
    auto op = insn->opcode();
    if (!insn->has_field()) {
      return;
    }
    auto field = resolve_field(insn->get_field());
    if (field == nullptr) {
      return;
    }
    if (is_sget(op) || is_iget(op)) {
      ++field_stats[field].reads;
      if (!is_own_init(field, method)) {
        ++field_stats[field].reads_outside_init;
      }
    } else if (is_sput(op) || is_iput(op)) {
      ++field_stats[field].writes;
    }
  });
  return field_stats;
}

} // namespace field_op_tracker
