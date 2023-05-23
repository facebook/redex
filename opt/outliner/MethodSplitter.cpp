/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodSplitter.h"

#include "ClosureAggregator.h"
#include "ConcurrentContainers.h"
#include "DexLimits.h"
#include "InitClassesWithSideEffects.h"
#include "MethodClosures.h"
#include "Show.h"
#include "SplittableClosures.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

namespace {

using namespace method_splitting_impl;

class DexState {
 private:
  std::unordered_set<const DexType*> m_type_refs;
  size_t m_method_refs_count;
  size_t max_type_refs;

 public:
  DexState() = delete;
  DexState(const DexState&) = delete;
  DexState& operator=(const DexState&) = delete;
  DexState(int32_t min_sdk,
           const init_classes::InitClassesWithSideEffects&
               init_classes_with_side_effects,
           DexClasses& dex,
           size_t reserved_trefs,
           size_t reserved_mrefs) {
    std::unordered_set<DexMethodRef*> method_refs;
    std::vector<DexType*> init_classes;
    for (auto cls : dex) {
      cls->gather_methods(method_refs);
      cls->gather_types(m_type_refs);
      cls->gather_init_classes(init_classes);
    }
    m_method_refs_count = method_refs.size() + reserved_mrefs;

    std::unordered_set<DexType*> refined_types;
    for (auto type : init_classes) {
      auto refined_type = init_classes_with_side_effects.refine(type);
      if (refined_type) {
        m_type_refs.insert(const_cast<DexType*>(refined_type));
      }
    }
    max_type_refs = get_max_type_refs(min_sdk) - reserved_trefs;
  }

  bool can_insert_type_refs(const std::unordered_set<const DexType*>& types) {
    size_t inserted_count{0};
    for (auto t : types) {
      if (!m_type_refs.count(t)) {
        inserted_count++;
      }
    }
    // Yes, looks a bit quirky, but matching what happens in
    // InterDex/DexStructure: The number of type refs must stay *below* the
    // maximum, and must never reach it.
    if ((m_type_refs.size() + inserted_count) >= max_type_refs) {
      TRACE(MS, 2, "[invoke sequence outliner] hit kMaxTypeRefs");
      return false;
    }
    return true;
  }

  void insert_type_refs(const std::unordered_set<const DexType*>& types) {
    always_assert(can_insert_type_refs(types));
    m_type_refs.insert(types.begin(), types.end());
    always_assert(m_type_refs.size() < max_type_refs);
  }

  bool can_insert_method_ref() {
    if (m_method_refs_count >= kMaxMethodRefs) {
      TRACE(MS, 2, "[invoke sequence outliner] hit kMaxMethodRefs");
      return false;
    }
    return true;
  }

  void insert_method_ref() {
    always_assert(can_insert_method_ref());
    m_method_refs_count++;
    always_assert(m_method_refs_count <= kMaxMethodRefs);
  }
};

DexMethod* split_method(const SplittableClosure& splittable_closure,
                        const std::string& name_infix,
                        size_t index,
                        DexState* dex_state) {
  if (!dex_state->can_insert_method_ref()) {
    return nullptr;
  }

  std::vector<DexType*> arg_types;
  for (auto& arg : splittable_closure.args) {
    if (arg.type) {
      arg_types.push_back(const_cast<DexType*>(arg.type));
    }
  }

  std::unordered_set<const DexType*> arg_types_set(arg_types.begin(),
                                                   arg_types.end());
  if (!dex_state->can_insert_type_refs(arg_types_set)) {
    return nullptr;
  }

  dex_state->insert_method_ref();
  dex_state->insert_type_refs(arg_types_set);

  auto method = splittable_closure.method_closures->method;
  std::string_view base_name = method::is_init(method)     ? "$init$"
                               : method::is_clinit(method) ? "$clinit$"
                                                           : method->str();
  auto* split_name = DexString::make_string(
      base_name + "$split$" + describe(splittable_closure.hot_split_kind) +
      name_infix + std::to_string(index));
  auto code = method->get_code();
  auto& cfg = code->cfg();

  auto split_code =
      std::make_unique<IRCode>(std::make_unique<cfg::ControlFlowGraph>());
  auto& split_cfg = split_code->cfg();
  split_code->set_debug_item(std::make_unique<DexDebugItem>());
  cfg.deep_copy(&split_cfg);
  auto split_entry_block = split_cfg.create_block();
  for (auto& arg : splittable_closure.args) {
    if (arg.type) {
      split_entry_block->push_back(
          (new IRInstruction(opcode::load_opcode(arg.type)))
              ->set_dest(arg.reg));
    }
  }
  for (auto& arg : splittable_closure.args) {
    if (!arg.type) {
      if (arg.def->has_move_result_pseudo()) {
        split_entry_block->push_back(new IRInstruction(*arg.def));
        split_entry_block->push_back(
            (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                ->set_dest(arg.reg));
      } else {
        split_entry_block->push_back(
            (new IRInstruction(*arg.def))->set_dest(arg.reg));
      }
    }
  }
  split_cfg.set_entry_block(split_entry_block);
  cfg::Block* launchpad_template;
  cfg::Block* split_landingpad;
  if (splittable_closure.closures.size() == 1) {
    always_assert(!splittable_closure.switch_block);
    auto* closure = splittable_closure.closures.front();
    launchpad_template = closure->target;
    split_landingpad = split_cfg.get_block(closure->target->id());
  } else {
    always_assert(splittable_closure.switch_block);
    split_landingpad =
        split_cfg.get_block(splittable_closure.switch_block->id());
    auto switch_it = split_landingpad->get_last_insn();
    always_assert(opcode::is_switch(switch_it->insn->opcode()));
    for (auto it = split_landingpad->begin(); it != switch_it;) {
      // Positions might be needed for parent references, we need to keep them
      if (it->type != MFLOW_POSITION) {
        it = split_landingpad->remove_mie(it);
      } else {
        it++;
      }
    }
    std::unordered_set<cfg::BlockId> split_target_ids;
    always_assert(!splittable_closure.closures.empty());
    launchpad_template = splittable_closure.closures.front()->target;
    for (auto* closure : splittable_closure.closures) {
      split_target_ids.insert(closure->target->id());
    }
    split_cfg.delete_succ_edge_if(split_landingpad, [&](auto* e) {
      return !split_target_ids.count(e->target()->id());
    });
    cfg.delete_succ_edge_if(splittable_closure.switch_block, [&](auto* e) {
      return e->type() == cfg::EDGE_BRANCH &&
             split_target_ids.count(e->target()->id());
    });
  }
  split_cfg.add_edge(split_entry_block, split_landingpad, cfg::EDGE_GOTO);

  auto type = method->get_class();
  auto proto = method->get_proto();
  auto split_type_list = DexTypeList::make_type_list(std::move(arg_types));
  auto split_proto = DexProto::make_proto(proto->get_rtype(), split_type_list);
  auto split_method_ref = DexMethod::make_method(type, split_name, split_proto);
  DexAccessFlags split_access_flags =
      DexAccessFlags::ACC_PRIVATE | DexAccessFlags::ACC_STATIC;
  auto split_method = split_method_ref->make_concrete(
      split_access_flags, std::move(split_code), /* is_virtual */ false);
  type_class(type)->add_method(split_method);

  split_method->set_deobfuscated_name(show_deobfuscated(split_method));
  split_method->rstate.set_dont_inline(); // Don't undo our work.
  if (method->rstate.too_large_for_inlining_into()) {
    split_method->rstate.set_too_large_for_inlining_into();
  }

  auto make_new_sb = [&](auto* method, auto& template_sb) {
    auto new_sb = std::make_unique<SourceBlock>(*template_sb);
    std::optional<SourceBlock::Val> opt_val;
    // TODO: For the hot case, compute "maximum" val over all closures.
    if (splittable_closure.hot_split_kind != HotSplitKind::Hot) {
      opt_val = SourceBlock::Val{0, 0};
    }
    source_blocks::fill_source_block(*new_sb, method, SourceBlock::kSyntheticId,
                                     opt_val);
    return new_sb;
  };
  // When splitting many cases out of a switch, we keep the positions of the
  // switch block, but not the source-block, so we insert a synthetic one here.
  if (splittable_closure.switch_block) {
    if (auto template_sb = source_blocks::get_first_source_block(
            splittable_closure.switch_block)) {
      auto split_landingpad_it = split_landingpad->get_first_insn();
      split_landingpad->insert_before(split_landingpad_it,
                                      make_new_sb(split_method, template_sb));
    }
  }
  // TODO: (Un)scale all source blocks in split method

  auto launchpad = cfg.create_block();
  auto copy = launchpad_template->preds();
  for (auto* e : copy) {
    if (!splittable_closure.switch_block ||
        e->src() == splittable_closure.switch_block) {
      cfg.set_edge_target(e, launchpad);
    }
  }
  auto invoke_insn = (new IRInstruction(OPCODE_INVOKE_STATIC))
                         ->set_method(split_method)
                         ->set_srcs_size(split_type_list->size());
  src_index_t i = 0;
  for (auto& arg : splittable_closure.args) {
    if (arg.type) {
      invoke_insn->set_src(i++, arg.reg);
    }
  }
  launchpad->push_back(invoke_insn);
  if (proto->is_void()) {
    launchpad->push_back(new IRInstruction(OPCODE_RETURN_VOID));
  } else {
    auto rtype = proto->get_rtype();
    reg_t min_registers_size = type::is_wide_type(rtype) ? 2 : 1;
    cfg.set_registers_size(
        std::max(cfg.get_registers_size(), min_registers_size));
    launchpad->push_back(
        (new IRInstruction(opcode::move_result_for_invoke(split_method)))
            ->set_dest(0));
    launchpad->push_back(
        (new IRInstruction(opcode::return_opcode(rtype)))->set_src(0, 0));
  }

  // Add source-block and position to otherwise naked launchpad
  auto launchpad_it = launchpad->get_first_insn();
  if (auto template_sb =
          source_blocks::get_first_source_block(launchpad_template)) {
    launchpad->insert_before(launchpad_it, make_new_sb(method, template_sb));
  }
  auto new_pos = std::make_unique<DexPosition>(0);
  new_pos->bind(DexString::make_string(show_deobfuscated(method)),
                DexString::make_string("RedexGenerated"));
  cfg.insert_before(launchpad, launchpad_it, std::move(new_pos));

  return split_method;
}

std::unordered_set<DexMethod*> split_splittable_closures(
    const std::vector<DexClasses*>& dexen,
    int32_t min_sdk,
    const init_classes::InitClassesWithSideEffects&
        init_classes_with_side_effects,
    size_t reserved_trefs,
    size_t reserved_mrefs,
    const std::unordered_map<DexType*, std::vector<SplittableClosure>>&
        splittable_closures,
    const std::string& name_infix,
    ConcurrentMap<std::string, size_t>* uniquifiers,
    Stats* stats,
    std::unordered_map<DexClasses*, std::unique_ptr<DexState>>* dex_states,
    ConcurrentSet<DexMethod*>* concurrent_added_methods) {
  Timer t("split");
  ConcurrentSet<DexMethod*> concurrent_affected_methods;
  auto process_dex = [&](DexClasses* dex) {
    std::vector<const SplittableClosure*> ranked_splittable_closures;
    for (auto* cls : *dex) {
      auto it = splittable_closures.find(cls->get_type());
      if (it == splittable_closures.end()) {
        continue;
      }
      for (auto& c : it->second) {
        ranked_splittable_closures.push_back(&c);
      }
    }
    if (ranked_splittable_closures.empty()) {
      return;
    }

    auto& dex_state = dex_states->at(dex);
    if (!dex_state) {
      dex_state =
          std::make_unique<DexState>(min_sdk, init_classes_with_side_effects,
                                     *dex, reserved_trefs, reserved_mrefs);
    }

    std::sort(ranked_splittable_closures.begin(),
              ranked_splittable_closures.end(), [](auto* c, auto* d) {
                if (c->rank != d->rank) {
                  return c->rank > d->rank;
                }
                auto* c_method = c->method_closures->method;
                auto* d_method = d->method_closures->method;
                if (c_method != d_method) {
                  return compare_dexmethods(c_method, d_method);
                }
                if (c->is_switch() != d->is_switch()) {
                  return c->is_switch() < d->is_switch();
                }
                return c->id() > d->id();
              });
    std::unordered_set<DexMethod*> affected_methods;
    for (auto* splittable_closure : ranked_splittable_closures) {
      auto method = splittable_closure->method_closures->method;
      std::string id =
          method->get_class()->str() + "." + method->get_name()->str();
      size_t index{0};
      uniquifiers->update(id, [&index](auto, auto& v, bool) { index = v++; });
      auto new_method =
          split_method(*splittable_closure, name_infix, index, dex_state.get());
      if (!new_method) {
        stats->dex_limits_hit++;
        for (auto* m : affected_methods) {
          m->get_code()->cfg().remove_unreachable_blocks();
        }
        affected_methods.clear();
        break;
      }
      stats->added_code_size += splittable_closure->added_code_size;
      if (splittable_closure->closures.size() == 1) {
        stats->split_count_simple++;
      } else {
        stats->split_count_switches++;
        stats->split_count_switch_cases += splittable_closure->closures.size();
      }
      switch (splittable_closure->hot_split_kind) {
      case HotSplitKind::Hot:
        stats->hot_split_count++;
        break;
      case HotSplitKind::HotCold:
        stats->hot_cold_split_count++;
        break;
      case HotSplitKind::Cold:
        stats->cold_split_count++;
        break;
      default:
        not_reached();
      }
      affected_methods.insert(method);
      affected_methods.insert(new_method);
      concurrent_added_methods->insert(new_method);
    }
    concurrent_affected_methods.insert(affected_methods.begin(),
                                       affected_methods.end());
  };
  workqueue_run<DexClasses*>(process_dex, dexen);
  return concurrent_affected_methods.move_to_container();
}

} // namespace

namespace method_splitting_impl {

void split_methods_in_stores(DexStoresVector& stores,
                             int32_t min_sdk,
                             const Config& config,
                             bool create_init_class_insns,
                             size_t reserved_mrefs,
                             size_t reserved_trefs,
                             Stats* stats,
                             const std::string& name_infix) {
  auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, create_init_class_insns);

  std::unordered_set<DexMethod*> methods;
  std::vector<DexClasses*> dexen;
  std::unordered_map<DexClasses*, std::unique_ptr<DexState>> dex_states;
  for (auto& store : stores) {
    for (auto& dex : store.get_dexen()) {
      dexen.push_back(&dex);
      dex_states[&dex];
      walk::code(dex,
                 [&](DexMethod* method, IRCode&) { methods.insert(method); });
    }
  }

  size_t iteration{0};
  ConcurrentMap<std::string, size_t> uniquifiers;
  while (!methods.empty() && iteration < config.max_iteration) {
    TRACE(MS, 2, "=== iteration[%zu]", iteration);
    Timer t("iteration " + std::to_string(iteration++));
    auto splittable_closures = select_splittable_closures(methods, config);
    ConcurrentSet<DexMethod*> concurrent_added_methods;
    methods = split_splittable_closures(
        dexen, min_sdk, init_classes_with_side_effects, reserved_trefs,
        reserved_mrefs, splittable_closures, name_infix, &uniquifiers, stats,
        &dex_states, &concurrent_added_methods);
    stats->added_methods.insert(concurrent_added_methods.begin(),
                                concurrent_added_methods.end());
    TRACE(MS, 1, "[%zu] Split out %zu methods", iteration,
          concurrent_added_methods.size());
  }
  walk::code(scope, [&](DexMethod* method, IRCode&) {
    method->rstate.reset_too_large_for_inlining_into();
  });
}

} // namespace method_splitting_impl
