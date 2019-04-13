/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnreadFields.h"

#include "DexClass.h"
#include "FieldOpTracker.h"
#include "IRCode.h"
#include "Resolver.h"
#include "Walkers.h"

namespace remove_unread_fields {

bool can_remove(DexField* field) {
  // XXX(jezng): why is can_rename not a subset of can_delete?
  return field != nullptr && !field->is_external() && can_delete(field) &&
         can_rename(field);
}

void PassImpl::run_pass(DexStoresVector& stores,
                        ConfigFiles& conf,
                        PassManager& mgr) {

  auto scope = build_class_scope(stores);
  field_op_tracker::FieldStatsMap field_stats = field_op_tracker::analyze(scope);

  uint32_t unread_fields = 0;
  for (auto& pair : field_stats) {
    auto* field = pair.first;
    auto& stats = pair.second;
    TRACE(RMUF,
          3,
          "%s: %lu %lu %lu %d\n",
          SHOW(field),
          stats.reads,
          stats.reads_outside_init,
          stats.writes,
          is_synthetic(field));
    if (stats.reads == 0 && can_remove(field)) {
      ++unread_fields;
    }
  }
  TRACE(RMUF, 2, "unread_fields %u\n", unread_fields);
  mgr.set_metric("unread_fields", unread_fields);

  // Remove the writes to unread fields.
  const auto& const_field_stats = field_stats;
  walk::parallel::code(
      scope, [&const_field_stats](const DexMethod*, IRCode& code) {
        for (auto& mie : InstructionIterable(code)) {
          auto insn = mie.insn;
          if (!insn->has_field()) {
            continue;
          }
          auto field = resolve_field(insn->get_field());
          if (!can_remove(field)) {
            continue;
          }
          auto it = const_field_stats.find(field);
          if (it == const_field_stats.end()) {
            continue;
          }
          if (it->second.reads == 0) {
            TRACE(RMUF, 5, "Removing %s\n", SHOW(insn));
            code.remove_opcode(code.iterator_to(mie));
          }
        }
      });
}

static PassImpl s_pass;

} // namespace remove_unread_fields
