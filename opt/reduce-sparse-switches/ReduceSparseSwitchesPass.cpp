/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReduceSparseSwitchesPass.h"

#include "BaselineProfile.h"
#include "CFGMutation.h"
#include "DexInstruction.h"
#include "DexLimits.h"
#include "InstructionLowering.h"
#include "InterDexPass.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_SPLITTING_TRANSFORMATIONS =
    "num_splitting_transformations";
constexpr const char* METRIC_SPLITTING_TRANSFORMATIONS_SWITCH_CASES =
    "num_splitting_transformations_switch_cases";
constexpr const char* METRIC_BINARY_SEARCH_TRANSFORMATIONS =
    "num_binary_search_transformations";
constexpr const char* METRIC_BINARY_SEARCH_TRANSFORMATIONS_SWITCH_CASES =
    "num_binary_search_transformations_switch_cases";
constexpr const char* METRIC_METHOD_REFS_EXCEEDED = "num_method_refs_exceeded";
constexpr const char* METRIC_FIELD_REFS_EXCEEDED = "num_field_refs_exceeded";

bool is_sufficiently_sparse(cfg::Block* block) {
  always_assert(opcode::is_switch(block->get_last_insn()->insn->opcode()));
  instruction_lowering::CaseKeysExtentBuilder ckeb;
  for (auto* e : block->succs()) {
    if (e->type() == cfg::EDGE_BRANCH) {
      ckeb.insert(*e->case_key());
    }
  }
  return ckeb->sufficiently_sparse();
}

static void split_sparse_switch_into_packed_and_sparse(
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    const IRList::iterator& switch_insn_it,
    const std::vector<int32_t>& case_keys,
    int64_t first,
    int64_t last) {
  // We now rewrite the switch from
  //   /* sparse */ switch (selector) {
  //     case K_0:          goto B_0;
  //       ...
  //     case K_{first-1}:  goto B_{first-1};
  //     case K_{first}:    goto B_{first};
  //       ...
  //     case K_{last}:     goto B_{last};
  //     case K_{last+1}:   goto B_{last+1};
  //       ...
  //     case K_{N-1}:      goto B_{N-1};
  //     default:           goto B_{default};
  //   }
  // to
  //   /* packed */ switch (selector) {
  //     K_{first}:             goto B_{first};
  //     ...
  //     K_{last}:              goto B_{last};
  //     default:
  //       /* usually sparse, but possibly packed */ switch (selector) {
  //         case K_0:          goto B_0;
  //           ...
  //         case K_{first-1}:  goto B_{first-1};
  //         case K_{last+1}:   goto B_{last+1};
  //           ...
  //         case K_{N-1}:      goto B_{N-1};
  //         default:           goto B_{default};
  //       }
  //   }

  auto selector_reg = switch_insn_it->insn->src(0);
  auto* goto_block = block->goes_to();
  auto* secondary_switch_block = cfg.create_block();
  auto succs_copy = block->succs();
  auto first_case_key = case_keys[first];
  auto last_case_key = case_keys[last];
  std::vector<std::pair<int32_t, cfg::Block*>> secondary_switch_case_to_block;
  secondary_switch_case_to_block.reserve(case_keys.size() - last + first - 1);
  std::vector<cfg::Edge*> sparse_edges;
  sparse_edges.reserve(case_keys.size() - last + first - 1);
  for (auto* e : succs_copy) {
    if (e->type() == cfg::EDGE_GOTO) {
      cfg.set_edge_target(e, secondary_switch_block);
      continue;
    }
    always_assert(e->type() == cfg::EDGE_BRANCH);
    auto case_key = *e->case_key();
    if (case_key >= first_case_key && case_key <= last_case_key) {
      continue;
    }
    secondary_switch_case_to_block.emplace_back(case_key, e->target());
    sparse_edges.push_back(e);
  }
  cfg.delete_edges(sparse_edges.begin(), sparse_edges.end());
  cfg.create_branch(
      secondary_switch_block,
      (new IRInstruction(OPCODE_SWITCH))->set_src(0, selector_reg), goto_block,
      secondary_switch_case_to_block);

  always_assert(!is_sufficiently_sparse(block));
}

// Create static field that holds the sorted sparse index array.
DexField* create_field(DexClass* cls,
                       size_t running_index,
                       size_t& field_refs) {
  auto array_type = DexType::make_type("[I");
  auto field_name = DexString::make_string(std::string("$sparse_index$") +
                                           std::to_string(running_index));
  DexField* field =
      DexField::make_field(cls->get_type(), field_name, array_type)
          ->make_concrete(ACC_PRIVATE | ACC_STATIC | ACC_VOLATILE);
  field->set_deobfuscated_name(show_deobfuscated(field));

  cls->add_field(field);
  field_refs++;

  return field;
}

// Create the method that initializes the static field that holds the sorted
// sparse index array.
DexMethod* make_fill_method(DexClass* cls,
                            DexField* field,
                            SourceBlock* template_sb,
                            const std::vector<cfg::Edge*>& ordered_branches,
                            size_t running_index,
                            size_t& method_refs) {
  std::vector<uint16_t> data_vec(4 + 2 * ordered_branches.size());
  data_vec[0] = FOPCODE_FILLED_ARRAY;
  data_vec[1] = 4;
  uint32_t size = ordered_branches.size();
  data_vec[2] = static_cast<uint16_t>(size & 0xFFFF);
  data_vec[3] = static_cast<uint16_t>(size >> 16);
  for (uint32_t i = 0; i < size; i++) {
    auto* e = ordered_branches[i];
    uint32_t v = *e->case_key();
    // Break integer value into two uint16_t values
    data_vec[4 + 2 * i] = static_cast<uint16_t>(v & 0xFFFF);
    data_vec[4 + 2 * i + 1] = static_cast<uint16_t>(v >> 16);
  }
  auto data = std::make_unique<DexOpcodeData>(data_vec);

  auto array_type = DexType::make_type("[I");
  auto code = std::make_unique<IRCode>();
  auto tmp = code->allocate_temp();
  if (template_sb) {
    auto new_sb = source_blocks::clone_as_synthetic(template_sb);
    code->push_back(std::move(new_sb));
  }
  code->push_back(
      (new IRInstruction(OPCODE_CONST))->set_literal(size)->set_dest(tmp));
  code->push_back((new IRInstruction(OPCODE_NEW_ARRAY))
                      ->set_type(array_type)
                      ->set_src(0, tmp));
  code->push_back(
      (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))->set_dest(tmp));
  code->push_back((new IRInstruction(OPCODE_FILL_ARRAY_DATA))
                      ->set_data(std::move(data))
                      ->set_src(0, tmp));
  code->push_back((new IRInstruction(OPCODE_SPUT_OBJECT))
                      ->set_field(field)
                      ->set_src(0, tmp));
  code->push_back((new IRInstruction(OPCODE_RETURN_OBJECT))->set_src(0, tmp));
  code->build_cfg();

  auto* proto =
      DexProto::make_proto(array_type, DexTypeList::make_type_list({}));
  auto* method_name = DexString::make_string(
      std::string("$sparse_index_init$") + std::to_string(running_index));
  auto* method = DexMethod::make_method(cls->get_type(), method_name, proto)
                     ->make_concrete(ACC_PRIVATE | ACC_STATIC, std::move(code),
                                     /* is_virtual */ false);
  method->rstate.set_generated();
  method->rstate.set_dont_inline();
  method->set_deobfuscated_name(show_deobfuscated(method));

  cls->add_method(method);
  method_refs++;

  DexAnnotationSet anno_set;
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      type::dalvik_annotation_optimization_NeverCompile(),
      DexAnnotationVisibility::DAV_BUILD));
  auto access = method->get_access();
  // attach_annotation_set requires the method to be synthetic.
  // A bit bizarre, and suggests that Redex' code to mutate annotations is
  // ripe for an overhaul. But I won't fight that here.
  method->set_access(access | ACC_SYNTHETIC);
  auto res = method->attach_annotation_set(
      std::make_unique<DexAnnotationSet>(anno_set));
  always_assert(res);
  method->set_access(access);

  TRACE(RSS, 4, "Created new init method {%s}:\n%s", SHOW(method),
        SHOW(method->get_code()->cfg()));
  return method;
}

// Rewrite a sparse switch to a binary search followed by a packed switch.
void rewrite_sparse_switch_to_binary_search(
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    const IRList::iterator& switch_insn_it,
    SourceBlock* template_sb,
    std::vector<cfg::Edge*>& ordered_branches,
    DexField* field,
    DexMethod* fill_method) {
  // Assuming we have an ordered sequence of case keys K_0, ..., K_{N-1}, we now
  // rewrite the switch from
  //   /* sparse */ switch (selector) {
  //     case K_0:          goto B_0;
  //       ...
  //     case K_{N-1}:      goto B_{N-1};
  //     default:           goto B_{default};
  //   }
  // to
  //   int[] a = $sparse_index$;
  //   if (a == nullptr) a = $sparse_index_init$();
  //   /* packed */ switch (Arrays.binarySearch(a, selector)) {
  //     0:           goto B_0;
  //     ...
  //     N-1:         goto B_{N-1};
  //     default:     goto B_{default};
  //   }

  auto* switch_insn = switch_insn_it->insn;
  auto tmp_reg = cfg.allocate_temp();

  auto* init_block = cfg.create_block();
  auto* earlier_block = cfg.split_block_before(block, switch_insn_it);
  if (cfg.entry_block() == block) {
    cfg.set_entry_block(earlier_block);
  }

  earlier_block->push_back(
      (new IRInstruction(OPCODE_SGET_OBJECT))->set_field(field));
  earlier_block->push_back(
      (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
          ->set_dest(tmp_reg));
  if (template_sb) {
    auto new_sb = source_blocks::clone_as_synthetic(template_sb, method);
    earlier_block->insert_before(
        source_blocks::find_first_block_insert_point(earlier_block),
        std::move(new_sb));
  }
  cfg.create_branch(earlier_block,
                    (new IRInstruction(OPCODE_IF_NEZ))->set_src(0, tmp_reg),
                    init_block, block);

  init_block->push_back(
      (new IRInstruction(OPCODE_INVOKE_STATIC))->set_method(fill_method));
  init_block->push_back(
      (new IRInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(tmp_reg));
  if (template_sb) {
    auto new_sb = source_blocks::clone_as_synthetic(template_sb, method);
    init_block->insert_before(
        source_blocks::find_first_block_insert_point(init_block),
        std::move(new_sb));
  }
  cfg.add_edge(init_block, block, cfg::EDGE_GOTO);

  auto selector_reg = switch_insn->src(0);
  auto* binary_search_method =
      DexMethod::make_method("Ljava/util/Arrays;.binarySearch:([II)I");
  block->push_front((new IRInstruction(OPCODE_MOVE_RESULT))->set_dest(tmp_reg));
  block->push_front((new IRInstruction(OPCODE_INVOKE_STATIC))
                        ->set_method(binary_search_method)
                        ->set_srcs_size(2)
                        ->set_src(0, tmp_reg)
                        ->set_src(1, selector_reg));
  switch_insn->set_src(0, tmp_reg);

  for (size_t i = 0; i < ordered_branches.size(); ++i) {
    ordered_branches[i]->set_case_key(i);
  }
}

double get_call_count(const method_profiles::MethodProfiles& method_profiles,
                      const DexMethod* method) {
  double call_count = 0;
  for (auto& [interaction_id, method_stats] :
       method_profiles.all_interactions()) {
    auto it = method_stats.find(method);
    if (it == method_stats.end()) {
      continue;
    }
    call_count += it->second.call_count;
  }
  return call_count;
}
} // namespace

// Find switches which can be split into packed and sparse switches, and apply
// the transformation.
ReduceSparseSwitchesPass::Stats
ReduceSparseSwitchesPass::splitting_transformation(size_t min_switch_cases,
                                                   DexMethod* method) {
  always_assert(min_switch_cases > 0);
  auto& cfg = method->get_code()->cfg();
  ReduceSparseSwitchesPass::Stats stats;
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    if (!opcode::is_switch(last_insn_it->insn->opcode())) {
      continue;
    }
    if (block->succs().size() - 1 < min_switch_cases) {
      continue;
    }
    if (!is_sufficiently_sparse(block)) {
      continue;
    }

    // The (ordered) switch case keys are a (monotonically) increasing sequence
    // of numbers K_0, ..., K_{N-1} (where N is the number of switch cases). We
    // try to find a maximal subsequence K_{first}, ..., K_{last} such that a
    // switch with these numbers is not sparse.
    std::vector<int32_t> case_keys;
    case_keys.reserve(block->succs().size() - 1);
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_BRANCH) {
        case_keys.push_back(*e->case_key());
      }
    }
    always_assert(case_keys.size() + 1 == block->succs().size());
    always_assert(!case_keys.empty());
    std::sort(case_keys.begin(), case_keys.end());
    int64_t min_splitting_size = (case_keys.size() + 1) / 2;
    always_assert(min_splitting_size > 0);
    int64_t first = 0;
    int64_t last = case_keys.size() - 1;
    while (last - first + 1 >= min_splitting_size &&
           instruction_lowering::CaseKeysExtent{
               case_keys[first], case_keys[last], (uint32_t)(last - first + 1)}
               .sufficiently_sparse()) {
      // The way we defined min_splitting_size implies that the number of packed
      // switch case keys we are looking for is at least half the size of the
      // original switch. Thus, the middle case key must be part of the packed
      // switch case keys. We use this fact to decide which extreme case key to
      // eliminate. However, we need to do some extra gymnastics to deal with
      // the case of an even switch size where there is no middle case key.
      auto mid_case_key2 = (case_keys[(first + last) / 2] +
                            (int64_t)case_keys[(first + last + 1) / 2]);
      if (mid_case_key2 - 2 * (int64_t)case_keys[first] >
          2 * (int64_t)case_keys[last] - mid_case_key2) {
        first++;
      } else {
        last--;
      }
    }
    if (last - first + 1 < min_splitting_size) {
      continue;
    }

    split_sparse_switch_into_packed_and_sparse(cfg, block, last_insn_it,
                                               case_keys, first, last);

    stats.splitting_transformations++;
    stats.splitting_transformations_switch_cases += last - first + 1;
  }
  return stats;
}

ReduceSparseSwitchesPass::Stats
ReduceSparseSwitchesPass::binary_search_transformation(
    size_t min_switch_cases,
    DexClass* cls,
    DexMethod* method,
    size_t& running_index,
    size_t& method_refs,
    size_t& field_refs,
    InsertOnlyConcurrentMap<DexMethod*, DexMethod*>* init_methods) {
  auto& cfg = method->get_code()->cfg();
  ReduceSparseSwitchesPass::Stats stats;
  cfg::CFGMutation mutation(cfg);
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    if (!opcode::is_switch(last_insn_it->insn->opcode())) {
      continue;
    }
    if (block->succs().size() - 1 < min_switch_cases) {
      continue;
    }
    if (!is_sufficiently_sparse(block)) {
      continue;
    }

    if (method_refs >= kMaxMethodRefs) {
      stats.method_refs_exceeded++;
      continue;
    }

    if (field_refs >= kMaxFieldRefs) {
      stats.field_refs_exceeded++;
      continue;
    }

    std::vector<cfg::Edge*> ordered_branches;
    ordered_branches.reserve(block->succs().size() - 1);
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_BRANCH) {
        ordered_branches.push_back(e);
      }
    }
    std::sort(
        ordered_branches.begin(), ordered_branches.end(),
        [](auto* e1, auto* e2) { return e1->case_key() < e2->case_key(); });

    auto* field = create_field(cls, running_index, field_refs);
    auto* template_sb =
        source_blocks::get_last_source_block_before(block, last_insn_it);
    auto* fill_method = make_fill_method(
        cls, field, template_sb, ordered_branches, running_index, method_refs);
    rewrite_sparse_switch_to_binary_search(method, cfg, block, last_insn_it,
                                           template_sb, ordered_branches, field,
                                           fill_method);

    running_index++;
    stats.binary_search_transformations++;
    stats.binary_search_transformations_switch_cases += block->succs().size();
    init_methods->emplace(fill_method, method);
  }
  mutation.flush();
  return stats;
}

void ReduceSparseSwitchesPass::bind_config() {
  bind("min_splitting_switch_cases",
       m_config.min_splitting_switch_cases,
       m_config.min_splitting_switch_cases);

  bind("min_binary_search_switch_cases_cold_per_call",
       m_config.min_binary_search_switch_cases_cold_per_call,
       m_config.min_binary_search_switch_cases_cold_per_call);
  bind("min_binary_search_switch_cases_hot_per_call",
       m_config.min_binary_search_switch_cases_hot_per_call,
       m_config.min_binary_search_switch_cases_hot_per_call);
  bind("min_binary_search_switch_cases",
       m_config.min_binary_search_switch_cases,
       m_config.min_binary_search_switch_cases);
};

void ReduceSparseSwitchesPass::eval_pass(DexStoresVector&,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  m_reserved_refs_handle = mgr.reserve_refs(name(),
                                            ReserveRefsInfo(/* frefs */ 0,
                                                            /* trefs */ 2,
                                                            /* mrefs */ 1));
}

void ReduceSparseSwitchesPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  always_assert(m_reserved_refs_handle);
  mgr.release_reserved_refs(*m_reserved_refs_handle);
  m_reserved_refs_handle = std::nullopt;

  // Don't run under instrumentation.
  if (mgr.get_redex_options().instrument_pass_enabled) {
    return;
  }

  auto& method_profiles = conf.get_method_profiles();
  std::optional<baseline_profiles::BaselineProfile> baseline_profile;
  if (m_config.min_binary_search_switch_cases_hot_per_call !=
      m_config.min_binary_search_switch_cases_cold_per_call) {
    const auto& baseline_profile_config =
        conf.get_default_baseline_profile_config();
    baseline_profile = std::make_optional<baseline_profiles::BaselineProfile>(
        baseline_profiles::get_baseline_profile(baseline_profile_config,
                                                method_profiles));
  }

  std::vector<DexClasses*> dexen;
  for (auto& store : stores) {
    for (auto& dex : store.get_dexen()) {
      dexen.push_back(&dex);
    }
  }

  const auto& interdex_metrics = mgr.get_interdex_metrics();
  auto get_interdex_metric = [&](const std::string& metric) {
    auto it = interdex_metrics.find(metric);
    return it == interdex_metrics.end() ? 0 : it->second;
  };
  size_t reserved_mrefs = get_interdex_metric(interdex::METRIC_RESERVED_MREFS);
  size_t reserved_frefs = get_interdex_metric(interdex::METRIC_RESERVED_FREFS);

  InsertOnlyConcurrentMap<DexMethod*, DexMethod*> init_methods;
  Stats stats;
  std::mutex stats_mutex;
  auto process_dex = [&](DexClasses* dex) {
    std::unordered_set<DexMethodRef*> method_refs;
    std::unordered_set<DexFieldRef*> field_refs;
    for (auto cls : *dex) {
      cls->gather_methods(method_refs);
      cls->gather_fields(field_refs);
    }
    auto method_refs_count = method_refs.size() + reserved_mrefs;
    auto field_refs_count = field_refs.size() + reserved_frefs;

    for (auto* cls : *dex) {
      size_t running_index = 0;
      for (auto* method : cls->get_all_methods()) {
        const auto code = method->get_code();
        if (!code || method->rstate.no_optimizations() ||
            method->rstate.should_not_outline()) {
          continue;
        }

        Stats local_stats;
        size_t last_splitting_transformations = 0;
        do {
          last_splitting_transformations =
              local_stats.splitting_transformations;
          local_stats += splitting_transformation(
              m_config.min_splitting_switch_cases, method);
        } while (last_splitting_transformations !=
                 local_stats.splitting_transformations);

        auto min_binary_search_switch_cases_per_call =
            m_config.min_binary_search_switch_cases_cold_per_call;
        if (baseline_profile) {
          auto it = baseline_profile->methods.find(method);
          if (it != baseline_profile->methods.end() && it->second.hot) {
            min_binary_search_switch_cases_per_call =
                m_config.min_binary_search_switch_cases_hot_per_call;
          }
        }

        // Unfortunately, we don't have block-level hit count information, so we
        // look at the call-count of the containing method. We ignore whether
        // the switch itself was actually ever hit, as even if it wasn't hit,
        // the transformation will still reduce the size of the method,
        // effectively doing a form of hot/cold splitting if the containing
        // method was hot.
        auto call_count = get_call_count(method_profiles, method);
        auto min_binary_search_switch_cases =
            call_count > 1
                ? std::max((uint64_t)(min_binary_search_switch_cases_per_call /
                                      call_count),
                           m_config.min_binary_search_switch_cases)
                : min_binary_search_switch_cases_per_call;

        local_stats += binary_search_transformation(
            min_binary_search_switch_cases, cls, method, running_index,
            method_refs_count, field_refs_count, &init_methods);
        if (local_stats.splitting_transformations == 0 &&
            local_stats.binary_search_transformations == 0) {
          continue;
        }

        TRACE(RSS, 3,
              "[reduce gotos] Split %zu sparse switches involving %zu switch "
              "cases, replaced %zu with a binary search involving %zu switch "
              "cases in {%s}",
              local_stats.splitting_transformations,
              local_stats.splitting_transformations_switch_cases,
              local_stats.binary_search_transformations,
              local_stats.binary_search_transformations_switch_cases,
              SHOW(method));
        TRACE(RSS, 4, "Rewrote {%s}:\n%s", SHOW(method), SHOW(code->cfg()));

        std::lock_guard<std::mutex> lock_guard(stats_mutex);
        stats += local_stats;
      }
    }
  };
  workqueue_run<DexClasses*>(process_dex, dexen);

  std::vector<DexMethod*> ordered_init_methods;
  ordered_init_methods.reserve(init_methods.size());
  for (auto&& [init_method, _] : init_methods) {
    ordered_init_methods.push_back(init_method);
  }
  std::sort(ordered_init_methods.begin(), ordered_init_methods.end(),
            compare_dexmethods);
  for (auto* init_method : ordered_init_methods) {
    std::vector<DexMethod*> source_methods{init_methods.at_unsafe(init_method)};
    method_profiles.derive_stats(init_method, source_methods);
  }

  mgr.incr_metric(METRIC_SPLITTING_TRANSFORMATIONS,
                  stats.splitting_transformations);
  mgr.incr_metric(METRIC_SPLITTING_TRANSFORMATIONS_SWITCH_CASES,
                  stats.splitting_transformations_switch_cases);
  mgr.incr_metric(METRIC_BINARY_SEARCH_TRANSFORMATIONS,
                  stats.binary_search_transformations);
  mgr.incr_metric(METRIC_BINARY_SEARCH_TRANSFORMATIONS_SWITCH_CASES,
                  stats.binary_search_transformations_switch_cases);
  mgr.incr_metric(METRIC_METHOD_REFS_EXCEEDED, stats.method_refs_exceeded);
  mgr.incr_metric(METRIC_FIELD_REFS_EXCEEDED, stats.field_refs_exceeded);

  TRACE(RSS, 1,
        "[reduce gotos] Replaced %zu sparse switches with a binary search, "
        "involving %zu switch cases; %zu/%zu method/field refs exceeded",
        stats.binary_search_transformations,
        stats.binary_search_transformations_switch_cases,
        stats.method_refs_exceeded, stats.field_refs_exceeded);
}

ReduceSparseSwitchesPass::Stats& ReduceSparseSwitchesPass::Stats::operator+=(
    const ReduceSparseSwitchesPass::Stats& that) {
  splitting_transformations += that.splitting_transformations;
  splitting_transformations_switch_cases +=
      that.splitting_transformations_switch_cases;
  binary_search_transformations += that.binary_search_transformations;
  binary_search_transformations_switch_cases +=
      that.binary_search_transformations_switch_cases;
  method_refs_exceeded += that.method_refs_exceeded;
  field_refs_exceeded += that.field_refs_exceeded;
  return *this;
}

static ReduceSparseSwitchesPass s_pass;
