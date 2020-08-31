/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <atomic>
#include <fstream>
#include <iostream>

#include "RemoveUnusedFields.h"

#include "CFGMutation.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "FieldOpTracker.h"
#include "IRCode.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Walkers.h"

using namespace remove_unused_fields;

namespace {

bool can_remove(DexField* field) {
  // XXX(jezng): why is can_rename not a subset of can_delete?
  return field != nullptr && !field->is_external() && can_delete(field) &&
         can_rename(field);
}

bool has_non_zero_static_value(DexField* field) {
  auto ev = field->get_static_value();
  return ev != nullptr && !ev->is_zero();
}

class RemoveUnusedFields final {
 public:
  RemoveUnusedFields(const Config& config, const Scope& scope)
      : m_config(config),
        m_scope(scope),
        m_remove_unread_field_put_types_allowlist(
            {type::java_lang_String(), type::java_lang_Class(),
             type::java_lang_Boolean(), type::java_lang_Byte(),
             type::java_lang_Short(), type::java_lang_Character(),
             type::java_lang_Integer(), type::java_lang_Long(),
             type::java_lang_Float(), type::java_lang_Double()}),
        m_java_lang_Enum(type::java_lang_Enum()) {
    analyze();
    transform();
  }

  const std::unordered_set<const DexField*>& unread_fields() const {
    return m_unread_fields;
  }

  const std::unordered_set<const DexField*>& unwritten_fields() const {
    return m_unwritten_fields;
  }

  const std::unordered_set<const DexField*>& zero_written_fields() const {
    return m_zero_written_fields;
  }

  const std::unordered_set<const DexField*>& vestigial_objects_written_fields()
      const {
    return m_vestigial_objects_written_fields;
  }

  size_t unremovable_unread_field_puts() const {
    return m_unremovable_unread_field_puts;
  }

 private:
  bool is_blocklisted(const DexField* field) const {
    return m_config.blocklist_types.count(field->get_type()) != 0 ||
           m_config.blocklist_classes.count(field->get_class()) != 0;
  }

  bool is_allowlisted(DexField* field) const {
    return !m_config.allowlist || m_config.allowlist->count(field) != 0;
  }

  bool can_remove_unread_field_put(DexField* field) const {
    auto t = field->get_type();
    // When no non-null value is ever written to a field, then it can never hold
    // a non-null reference
    if (m_zero_written_fields.count(field)) {
      return true;
    }

    // Fields of primitive types can never hold on to references
    if (type::is_primitive(t)) {
      return true;
    }

    // Nobody should ever rely on the lifetime of strings, classes, boxed
    // values, or enum values
    if (m_remove_unread_field_put_types_allowlist.count(t)) {
      return true;
    }
    if (type::is_subclass(m_java_lang_Enum, t)) {
      return true;
    }

    // We don't have to worry about lifetimes of harmless objects
    if (m_vestigial_objects_written_fields.count(field)) {
      return true;
    }

    return false;
  }

  void analyze() {
    field_op_tracker::FieldStatsMap field_stats =
        field_op_tracker::analyze(m_scope);

    // analyze_non_zero_writes and the later transform() need (editable) cfg
    walk::parallel::code(m_scope, [&](const DexMethod*, IRCode& code) {
      code.build_cfg(/* editable = true*/);
    });

    boost::optional<field_op_tracker::FieldWrites> field_writes;
    if (m_config.remove_zero_written_fields ||
        m_config.remove_vestigial_objects_written_fields) {
      field_writes = field_op_tracker::analyze_writes(m_scope);
    }

    for (auto& pair : field_stats) {
      auto* field = pair.first;
      auto& stats = pair.second;
      TRACE(RMUF,
            3,
            "%s: %lu %lu %lu %d",
            SHOW(field),
            stats.reads,
            stats.reads_outside_init,
            stats.writes,
            is_synthetic(field));
      if (can_remove(field) && !is_blocklisted(field) &&
          is_allowlisted(field)) {
        if (m_config.remove_unread_fields && stats.reads == 0) {
          m_unread_fields.emplace(field);
          if (m_config.remove_vestigial_objects_written_fields &&
              !field_writes->non_vestigial_objects_written_fields.count(
                  field)) {
            m_vestigial_objects_written_fields.emplace(field);
          }
        } else if (m_config.remove_unwritten_fields && stats.writes == 0 &&
                   !has_non_zero_static_value(field)) {
          m_unwritten_fields.emplace(field);
        } else if (m_config.remove_zero_written_fields &&
                   !field_writes->non_zero_written_fields.count(field) &&
                   !has_non_zero_static_value(field)) {
          m_zero_written_fields.emplace(field);
        }
      }
    }
    TRACE(RMUF, 2, "unread_fields %u", m_unread_fields.size());
    TRACE(RMUF, 2, "unwritten_fields %u", m_unwritten_fields.size());
    TRACE(RMUF, 2, "zero written_fields %u", m_zero_written_fields.size());
    TRACE(RMUF,
          2,
          "vestigial objects written_fields %u",
          m_vestigial_objects_written_fields.size());
  }

  void transform() {
    // Replace reads to unwritten fields with appropriate const-0 instructions,
    // and remove the writes to unread fields.
    walk::parallel::code(m_scope, [&](const DexMethod*, IRCode& code) {
      auto& cfg = code.cfg();
      cfg::CFGMutation m(cfg);
      auto iterable = cfg::InstructionIterable(cfg);
      for (auto insn_it = iterable.begin(); insn_it != iterable.end();
           ++insn_it) {
        auto* insn = insn_it->insn;
        if (!insn->has_field()) {
          continue;
        }
        auto field = resolve_field(insn->get_field());
        bool replace_insn = false;
        bool remove_insn = false;
        if (m_unread_fields.count(field)) {
          if (m_config.unsafe || can_remove_unread_field_put(field)) {
            always_assert(opcode::is_an_iput(insn->opcode()) ||
                          opcode::is_an_sput(insn->opcode()));
            TRACE(RMUF, 5, "Removing %s", SHOW(insn));
            remove_insn = true;
          } else {
            m_unremovable_unread_field_puts++;
          }
        } else if (m_unwritten_fields.count(field)) {
          always_assert(opcode::is_an_iget(insn->opcode()) ||
                        opcode::is_an_sget(insn->opcode()));
          TRACE(RMUF, 5, "Replacing %s with const 0", SHOW(insn));
          replace_insn = true;
        } else if (m_zero_written_fields.count(field)) {
          if (opcode::is_an_iput(insn->opcode()) ||
              opcode::is_an_sput(insn->opcode())) {
            TRACE(RMUF, 5, "Removing %s", SHOW(insn));
            remove_insn = true;
          } else {
            always_assert(opcode::is_an_iget(insn->opcode()) ||
                          opcode::is_an_sget(insn->opcode()));
            TRACE(RMUF, 5, "Replacing %s with const 0", SHOW(insn));
            replace_insn = true;
          }
        }
        if (replace_insn) {
          auto move_result = cfg.move_result_of(insn_it);
          if (move_result.is_end()) {
            continue;
          }
          auto write_insn = move_result->insn;
          IRInstruction* const0 = new IRInstruction(
              write_insn->dest_is_wide() ? OPCODE_CONST_WIDE : OPCODE_CONST);
          const0->set_dest(write_insn->dest())->set_literal(0);
          m.replace(insn_it, {const0});
        } else if (remove_insn) {
          m.remove(insn_it);
        }
      }
      m.flush();
      code.clear_cfg();
    });
  }

  const Config& m_config;
  const Scope& m_scope;
  std::unordered_set<const DexField*> m_unread_fields;
  std::unordered_set<const DexField*> m_unwritten_fields;
  std::unordered_set<const DexField*> m_zero_written_fields;
  std::unordered_set<const DexField*> m_vestigial_objects_written_fields;
  std::unordered_set<DexType*> m_remove_unread_field_put_types_allowlist;
  DexType* m_java_lang_Enum;
  std::atomic<size_t> m_unremovable_unread_field_puts{0};
};

} // namespace

namespace remove_unused_fields {

void PassImpl::run_pass(DexStoresVector& stores,
                        ConfigFiles& conf,
                        PassManager& mgr) {
  auto scope = build_class_scope(stores);
  RemoveUnusedFields rmuf(m_config, scope);
  mgr.set_metric("unread_fields", rmuf.unread_fields().size());
  mgr.set_metric("unwritten_fields", rmuf.unwritten_fields().size());
  mgr.set_metric("zero_written_fields", rmuf.zero_written_fields().size());
  mgr.set_metric("vestigial_objects_written_fields",
                 rmuf.vestigial_objects_written_fields().size());
  mgr.set_metric("unremovable_unread_field_puts",
                 rmuf.unremovable_unread_field_puts());

  if (m_export_removed) {
    std::vector<const DexField*> removed_fields(rmuf.unread_fields().begin(),
                                                rmuf.unread_fields().end());
    removed_fields.insert(removed_fields.end(),
                          rmuf.unwritten_fields().begin(),
                          rmuf.unwritten_fields().end());
    sort_unique(removed_fields, compare_dexfields);
    auto path = conf.metafile(REMOVED_FIELDS_FILENAME);
    std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
    for (auto* field : removed_fields) {
      ofs << show_deobfuscated(field) << "\n";
    }
  }
}

static PassImpl s_pass;

} // namespace remove_unused_fields
