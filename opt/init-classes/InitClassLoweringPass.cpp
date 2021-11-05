/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InitClassLoweringPass.h"

#include "CFGMutation.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "InitClassPruner.h"
#include "InitClassesWithSideEffects.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

using namespace init_classes;

namespace {

constexpr const char* METRIC_METHODS_WITH_INIT_CLASS =
    "methods_with_init_class";
constexpr const char* METRIC_FIELDS_ADDED = "fields_added";
constexpr const char* METRIC_INIT_CLASS_INSTRUCTIONS =
    "init_class_instructions";
constexpr const char* METRIC_INIT_CLASS_INSTRUCTIONS_REMOVED =
    "init_class_instructions_removed";
constexpr const char* METRIC_INIT_CLASS_INSTRUCTIONS_REFINED =
    "init_class_instructions_refined";
constexpr const char* METRIC_SGET_INSTRUCTIONS_ADDED =
    "sget_instructions_added";
constexpr const char* METRIC_INIT_CLASSES = "init_classes";
constexpr const char* METRIC_FIELDS_MADE_PUBLIC = "fields_made_public";
constexpr const char* METRIC_TYPES_MADE_PUBLIC = "types_made_public";

static const char* redex_field_name = "$redex_init_class";

class InitClassFields {
 public:
  DexField* get(DexType* type) {
    DexField* res = nullptr;
    m_init_class_fields.update(
        type, [&](DexType* type_, InitClassField& icf, bool exist) {
          if (!exist) {
            icf.field = make_init_class_field(type_);
            icf.field->rstate.set_init_class();
          }
          icf.count++;
          res = icf.field;
        });
    always_assert(res);
    return res;
  }

  size_t get_classes_size() { return m_init_class_fields.size(); }

  size_t get_fields_added() { return m_fields_added; }

  std::vector<std::pair<DexType*, size_t>>
  get_ordered_init_class_reference_counts() {
    std::vector<std::pair<DexType*, size_t>> res;
    for (auto& p : m_init_class_fields) {
      res.emplace_back(p.first, p.second.count);
    }
    std::stable_sort(res.begin(), res.end(), [](const auto& a, const auto& b) {
      return a.second > b.second;
    });
    return res;
  }

  std::vector<DexField*> get_all() {
    std::vector<DexField*> res;
    for (auto& p : m_init_class_fields) {
      res.emplace_back(p.second.field);
    }
    return res;
  }

 private:
  const DexString* m_field_name = DexString::make_string(redex_field_name);
  std::atomic<size_t> m_fields_added{0};
  struct InitClassField {
    DexField* field{nullptr};
    size_t count{0};
  };
  ConcurrentMap<DexType*, InitClassField> m_init_class_fields;

  DexField* make_init_class_field(DexType* type) {
    auto cls = type_class(type);
    always_assert(cls);
    const auto& sfields = cls->get_sfields();
    if (!sfields.empty()) {
      // We pick the first...
      // 1. non-wide primitive, if any.
      for (auto f : sfields) {
        if (!type::is_wide_type(f->get_type()) &&
            type::is_primitive(f->get_type())) {
          return f;
        }
      }
      // 2. non-wide, if any.
      for (auto f : sfields) {
        if (!type::is_wide_type(f->get_type())) {
          return f;
        }
      }
      // 3. anything.
      return sfields.front();
    }
    always_assert_log(DexField::get_field(type, m_field_name, type::_int()) ==
                          nullptr,
                      "field %s already exists!",
                      redex_field_name);
    auto field = DexField::make_field(type, m_field_name, type)
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    field->rstate.set_root();
    insert_sorted(cls->get_sfields(), field, compare_dexfields);
    field->set_deobfuscated_name(show_deobfuscated(field));
    m_fields_added++;
    return field;
  }
};

void make_public(const std::vector<DexField*>& fields,
                 size_t* fields_made_public,
                 size_t* types_made_public) {
  std::unordered_set<DexType*> visited;
  std::function<void(DexType*)> visit;
  visit = [&](DexType* type) {
    auto cls = type_class(type);
    if (cls && !cls->is_external() && !is_public(cls)) {
      set_public(cls);
      types_made_public++;
      visit(cls->get_super_class());
    }
  };
  for (auto& f : fields) {
    if (!is_public(f)) {
      set_public(f);
      fields_made_public++;
    }
    visit(f->get_class());
    visit(f->get_type());
  }
}
} // namespace

void InitClassLoweringPass::bind_config() {
  bind(
      "drop", m_drop, m_drop,
      "Whether to drop the init-class instructions, instead of lowering them.");
}

void InitClassLoweringPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  const auto scope = build_class_scope(stores);
  auto create_init_class_insns = conf.create_init_class_insns();
  TRACE(ICL, 1, "[InitClassLowering] create_init_class_insns: %d",
        create_init_class_insns);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, create_init_class_insns);
  std::atomic<size_t> sget_instructions_added{0};
  std::atomic<size_t> methods_with_init_class{0};
  InitClassFields init_class_fields;
  const auto stats =
      walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
        auto code = method->get_code();
        if (!code ||
            !method::count_opcode_of_types(code, {IOPCODE_INIT_CLASS})) {
          return Stats();
        }
        cfg::ScopedCFG cfg(code);
        InitClassPruner pruner(init_classes_with_side_effects,
                               method->get_class(), *cfg);
        pruner.apply();
        auto local_stats = pruner.get_stats();
        if (local_stats.init_class_instructions == 0) {
          return local_stats;
        }
        TRACE(ICL,
              6,
              "[InitClassLowering] method %s with %zu init-classes:\n%s",
              SHOW(method),
              local_stats.init_class_instructions,
              SHOW(*cfg));
        methods_with_init_class++;
        boost::optional<reg_t> tmp_reg;
        boost::optional<reg_t> wide_tmp_reg;
        auto get_reg = [&](DexField* field) {
          if (type::is_wide_type(field->get_type())) {
            if (!wide_tmp_reg) {
              wide_tmp_reg = cfg->allocate_wide_temp();
            }
            return *wide_tmp_reg;
          }
          if (!tmp_reg) {
            tmp_reg = cfg->allocate_temp();
          }
          return *tmp_reg;
        };
        cfg::CFGMutation mutation(*cfg);
        size_t local_sget_instructions_added = 0;
        for (auto block : cfg->blocks()) {
          auto ii = InstructionIterable(block);
          for (auto it = ii.begin(); it != ii.end(); it++) {
            if (it->insn->opcode() != IOPCODE_INIT_CLASS) {
              continue;
            }
            always_assert(create_init_class_insns);
            if (m_drop) {
              mutation.remove(block->to_cfg_instruction_iterator(it));
              continue;
            }
            auto field = init_class_fields.get(it->insn->get_type());
            reg_t reg = get_reg(field);
            auto sget_insn =
                (new IRInstruction(opcode::sget_opcode_for_field(field)))
                    ->set_field(field);
            auto move_result_insn =
                (new IRInstruction(
                     opcode::move_result_pseudo_for_sget(sget_insn->opcode())))
                    ->set_dest(reg);
            mutation.replace(block->to_cfg_instruction_iterator(it),
                             {sget_insn, move_result_insn});
            local_sget_instructions_added++;
          }
        }
        mutation.flush();
        if (local_sget_instructions_added) {
          sget_instructions_added += local_sget_instructions_added;
        }
        return local_stats;
      });

  TRACE(ICL, 1,
        "[InitClassLowering] %zu methods have %zu sget instructions; %zu "
        "classes with clinits with side effects needed initialization with "
        "%zu added fields",
        (size_t)methods_with_init_class, (size_t)sget_instructions_added,
        init_class_fields.get_classes_size(),
        init_class_fields.get_fields_added());

  if (traceEnabled(ICL, 5)) {
    for (auto& p :
         init_class_fields.get_ordered_init_class_reference_counts()) {
      auto cls = type_class(p.first);
      auto count = p.second;
      auto clinit = cls->get_clinit();
      always_assert(clinit);
      cfg::ScopedCFG cfg(clinit->get_code());
      TRACE(ICL, 5,
            "[InitClassLowering] clinit of %s referenced by %zu init-class "
            "instructions:\n%s",
            SHOW(cls), count, SHOW(*cfg));
    }
  }

  size_t fields_made_public = 0;
  size_t types_made_public = 0;
  make_public(init_class_fields.get_all(), &fields_made_public,
              &types_made_public);
  TRACE(ICL, 5,
        "[InitClassLowering] made %zu existing fields and %zu classes public",
        fields_made_public, types_made_public);

  mgr.incr_metric(METRIC_METHODS_WITH_INIT_CLASS, methods_with_init_class);
  mgr.incr_metric(METRIC_FIELDS_ADDED, init_class_fields.get_fields_added());
  mgr.incr_metric(METRIC_INIT_CLASS_INSTRUCTIONS,
                  stats.init_class_instructions);
  mgr.incr_metric(METRIC_INIT_CLASS_INSTRUCTIONS_REMOVED,
                  stats.init_class_instructions_removed);
  mgr.incr_metric(METRIC_INIT_CLASS_INSTRUCTIONS_REFINED,
                  stats.init_class_instructions_refined);
  mgr.incr_metric(METRIC_INIT_CLASSES, init_class_fields.get_classes_size());
  mgr.incr_metric(METRIC_SGET_INSTRUCTIONS_ADDED, sget_instructions_added);
  mgr.incr_metric(METRIC_FIELDS_MADE_PUBLIC, fields_made_public);
  mgr.incr_metric(METRIC_TYPES_MADE_PUBLIC, types_made_public);

  mgr.record_init_class_lowering();
}

static InitClassLoweringPass s_pass;
