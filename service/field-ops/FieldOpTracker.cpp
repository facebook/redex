/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FieldOpTracker.h"

#include "BaseIRAnalyzer.h"
#include "ConcurrentContainers.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReachingDefinitions.h"
#include "Resolver.h"
#include "Walkers.h"

namespace field_op_tracker {

bool is_own_init(DexField* field, const DexMethod* method) {
  return (method::is_clinit(method) || method::is_init(method)) &&
         method->get_class() == field->get_class();
}

FieldWrites analyze_writes(const Scope& scope) {
  ConcurrentSet<DexField*> concurrent_non_zero_written_fields;
  ConcurrentSet<DexField*> concurrent_non_vestigial_objects_written_fields;
  walk::parallel::code(scope, [&](const DexMethod*, const IRCode& code) {
    auto& cfg = code.cfg();

    // First, we'll build up some information about which values (represented by
    // instructions that create them) escape by being stored in fields, or,
    // represented as the nullptr field, escape otherwise.
    reaching_defs::MoveAwareFixpointIterator fp_iter(cfg);
    fp_iter.run({});
    std::unordered_map<IRInstruction*, std::unordered_set<DexField*>>
        escaped_non_zero_values;
    for (auto* block : cfg.blocks()) {
      auto env = fp_iter.get_entry_state_at(block);
      if (env.is_bottom()) {
        continue;
      }
      auto insns = InstructionIterable(block);
      for (auto it = insns.begin(); it != insns.end();
           fp_iter.analyze_instruction(it++->insn, &env)) {
        auto* insn = it->insn;
        auto op = insn->opcode();
        if (is_move(op) || is_iget(op) || is_aget(op) ||
            is_conditional_branch(op) || is_monitor(op) ||
            op == OPCODE_ARRAY_LENGTH || op == OPCODE_CHECK_CAST ||
            op == OPCODE_INSTANCE_OF || op == OPCODE_FILL_ARRAY_DATA) {
          // these operations don't let an object/array reference escape
          continue;
        }
        for (size_t src_idx = 0; src_idx < insn->srcs_size(); src_idx++) {
          DexField* put_value_field{nullptr};
          if (is_iput(op) || is_sput(op)) {
            if (src_idx == 1) {
              // writing to a field of an object doesn't let the object
              // reference escape
              continue;
            }
            put_value_field = resolve_field(insn->get_field());
          }
          if (is_aput(op) && op != OPCODE_APUT_OBJECT && src_idx == 1) {
            // writing to an element of an array doesn't let the array reference
            // escape; however, for aput-object, we'll register an escape of the
            // array definition, so that we'll prevent that array from being
            // regarded as vestigial
            continue;
          }
          auto src = insn->src(src_idx);
          auto src_defs = env.get(src);
          always_assert(!src_defs.is_bottom());
          always_assert(!src_defs.is_top());
          for (auto* def : src_defs.elements()) {
            if ((def->opcode() == OPCODE_CONST ||
                 def->opcode() == OPCODE_CONST_WIDE) &&
                def->get_literal() == 0) {
              continue;
            }
            escaped_non_zero_values[def].insert(put_value_field);
          }
        }
      }
    }

    // Next, we'll determine which fields are being written to with
    // (potentially) non-zero values, and which fields are being written to with
    // non-vestigial object. Right now, we only consider as vestigial objects
    // newly created arrays which do not contain any other objects and
    // which escape to a single field.
    // TODO: Also consider OPCODE_NEW_INSTANCE when the lifetime of the instance
    // does not matter. Note that all newly created objects would go through a
    // constructor call, which would need further escape analysis.
    std::unordered_set<DexField*> non_zero_written_fields;
    std::unordered_set<DexField*> non_vestigial_objects_written_fields;
    for (auto& p : escaped_non_zero_values) {
      auto& fields = p.second;
      auto insn = p.first;
      auto op = insn->opcode();
      auto is_vestigial_object =
          fields.size() == 1 && !fields.count(nullptr) &&
          (op == OPCODE_NEW_ARRAY ||
           (op == OPCODE_FILLED_NEW_ARRAY &&
            (!type::is_object(
                 type::get_array_component_type(insn->get_type())) ||
             insn->srcs_size() == 0)));
      for (auto field : fields) {
        if (field == nullptr) {
          continue;
        }
        non_zero_written_fields.insert(field);
        if (!is_vestigial_object && type::is_object(field->get_type())) {
          non_vestigial_objects_written_fields.insert(field);
        }
      }
    }
    for (auto f : non_zero_written_fields) {
      concurrent_non_zero_written_fields.insert(f);
    }
    for (auto f : non_vestigial_objects_written_fields) {
      concurrent_non_vestigial_objects_written_fields.insert(f);
    }
  });
  return FieldWrites{
      std::unordered_set<DexField*>(concurrent_non_zero_written_fields.begin(),
                                    concurrent_non_zero_written_fields.end()),
      std::unordered_set<DexField*>(
          concurrent_non_vestigial_objects_written_fields.begin(),
          concurrent_non_vestigial_objects_written_fields.end())};
};

FieldStatsMap analyze(const Scope& scope) {
  FieldStatsMap field_stats;
  // Gather the read/write counts from instructions.
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
  // Gather field reads from annotations.
  walk::annotations(scope, [&](DexAnnotation* anno) {
    std::vector<DexFieldRef*> fields_in_anno;
    anno->gather_fields(fields_in_anno);
    for (const auto& field_ref : fields_in_anno) {
      auto field = resolve_field(field_ref);
      if (field) {
        ++field_stats[field].reads;
      }
    }
  });
  return field_stats;
}

} // namespace field_op_tracker
