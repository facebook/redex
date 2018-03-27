/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveUnreadFields.h"

#include "Resolver.h"
#include "Walkers.h"

namespace remove_unread_fields {

struct FieldStats {
  // Number of instructions which read a field in the entire program.
  size_t reads{0};
  // Number of instructions which read this field outside of a <clinit> or
  // <init>. This number is not actually used to guide field removal decisions
  // for now.
  size_t reads_outside_init{0};
  // Number of instructions which write a field in the entire program.  This
  // number is not actually used to guide field removal decisions for now.
  size_t writes{0};
};

void PassImpl::run_pass(DexStoresVector& stores,
                        ConfigFiles& cfg,
                        PassManager& mgr) {

  auto scope = build_class_scope(stores);

  // Gather the read/write counts.
  std::unordered_map<DexField*, FieldStats> field_stats;
  walk::opcodes(scope, [&](const DexMethod* method, const IRInstruction* insn) {
    auto op = insn->opcode();
    if (!insn->has_field()) {
      return;
    }
    auto field = resolve_field(insn->get_field());
    if (field == nullptr) {
      return;
    }
    // XXX(jezng): why is can_rename not a subset of can_delete?
    if (field->is_external() || !can_delete(field) || !can_rename(field)) {
      return;
    }
    if (is_sget(op) || is_iget(op)) {
      ++field_stats[field].reads;
      if (!((is_clinit(method) || is_init(method)) &&
            method->get_class() == field->get_class())) {
        ++field_stats[field].reads_outside_init;
      }
    } else if (is_sput(op) || is_iput(op)) {
      ++field_stats[field].writes;
    }
  });

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
    if (stats.reads == 0) {
      mgr.incr_metric("unread_fields", 1);
    }
  }

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
          if (field == nullptr) {
            continue;
          }
          if (!const_field_stats.count(field)) {
            continue;
          }
          if (const_field_stats.at(field).reads == 0) {
            TRACE(RMUF, 5, "Removing %s\n", SHOW(insn));
            code.remove_opcode(code.iterator_to(mie));
          }
        }
      });
}

static PassImpl s_pass;

} // namespace remove_unread_fields
