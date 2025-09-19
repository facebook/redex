/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* This pass optionally creates a baseline profile file in a superset of the
 * human-readable ART profile format (HRF) according to
 * https://developer.android.com/topic/performance/baselineprofiles/manually-create-measure#define-rules-manually
 * .
 */

#include "ArtProfileWriterPass.h"

#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <fstream>
#include <string>

#include "BaselineProfile.h"
#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "DeterministicContainers.h"
#include "DexAssessments.h"
#include "DexStructure.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "LoopInfo.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "TypeInference.h"
#include "Walkers.h"

namespace {
const std::string BASELINE_PROFILES_FILE = "additional-baseline-profiles.list";
const std::string STORE_FENCE_HELPER_NAME = "Lredex/$StoreFenceHelper;";

// Helper function that checks if a block is not hit in any interaction.
bool is_cold(cfg::Block* b) {
  const auto* sb = source_blocks::get_first_source_block(b);
  if (sb == nullptr) {
    return true;
  }

  bool may_be_hot = false;
  sb->foreach_val_early([&may_be_hot](const auto& val) {
    may_be_hot = (!val || val->val > 0.0f);
    return may_be_hot;
  });

  return !may_be_hot;
}

bool is_sparse(cfg::Block* switch_block) {
  instruction_lowering::CaseKeysExtentBuilder ckeb;
  for (auto* e : switch_block->succs()) {
    if (e->type() == cfg::EDGE_BRANCH) {
      ckeb.insert(*e->case_key());
    }
  }
  return ckeb->sufficiently_sparse();
}

// Only certain "hot" methods get compiled.
bool is_compiled(DexMethod* method,
                 const baseline_profiles::MethodFlags& flags) {
  return flags.hot && !method::is_clinit(method);
}

bool is_compiled(const baseline_profiles::BaselineProfile& baseline_profile,
                 DexMethod* method) {
  auto it = baseline_profile.methods.find(method);
  return it != baseline_profile.methods.end() &&
         is_compiled(method, it->second);
}

bool is_simple(DexMethod* method, IRInstruction** invoke_insn = nullptr) {
  auto* code = method->get_code();
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  if (cfg.blocks().size() != 1) {
    return false;
  }
  auto* b = cfg.entry_block();
  auto last_it = b->get_last_insn();
  if (last_it == b->end() || !opcode::is_a_return(last_it->insn->opcode())) {
    return false;
  }
  auto ii = InstructionIterable(b);
  auto it = ii.begin();
  always_assert(it != ii.end());
  while (opcode::is_a_load_param(it->insn->opcode())) {
    ++it;
    always_assert(it != ii.end());
  }
  if (opcode::is_a_const(it->insn->opcode())) {
    ++it;
    always_assert(it != ii.end());
  } else if ((opcode::is_an_iget(it->insn->opcode()) ||
              opcode::is_an_sget(it->insn->opcode()))) {
    ++it;
    always_assert(it != ii.end());
  } else if (opcode::is_an_invoke(it->insn->opcode())) {
    if (invoke_insn != nullptr) {
      *invoke_insn = it->insn;
    }
    ++it;
    always_assert(it != ii.end());
  }
  if (opcode::is_move_result_any(it->insn->opcode())) {
    ++it;
    always_assert(it != ii.end());
  }
  always_assert(it != ii.end());
  return it->insn == last_it->insn;
}

void never_inline(bool attach_annotations,
                  float hot_block_appear_threshold,
                  float hot_method_appear_threshold,
                  const Scope& scope,
                  const baseline_profiles::BaselineProfile& baseline_profile,
                  PassManager& mgr,
                  const method_profiles::MethodProfiles& method_profiles) {
  DexAnnotationSet anno_set;
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      type::dalvik_annotation_optimization_NeverInline(),
      DexAnnotationVisibility::DAV_BUILD));

  auto consider_callee = [&](DexMethod* callee) {
    if (callee == nullptr || (callee->get_code() == nullptr)) {
      return false;
    }
    auto* cls = type_class(callee->get_class());
    if ((cls == nullptr) || cls->is_external()) {
      return false;
    }
    if (callee->is_virtual() && (!is_final(callee) && !is_final(cls))) {
      return false;
    }
    return true;
  };

  using ReceiverMap = UnorderedMap<const IRInstruction*, const DexType*>;
  InsertOnlyConcurrentMap<DexMethod*, ReceiverMap> receiver_types;
  auto get_callee = [&](DexMethod* caller,
                        IRInstruction* invoke_insn) -> DexMethod* {
    DexMethod* callee;
    do {
      callee = nullptr;
      auto caller_it = receiver_types.find(caller);
      if (caller_it != receiver_types.end()) {
        const auto& map = caller_it->second;
        auto map_it = map.find(invoke_insn);
        if (map_it != map.end()) {
          const auto* receiver_type = map_it->second;
          auto* receiver_cls = type_class(receiver_type);
          if ((receiver_cls != nullptr) && !is_interface(receiver_cls)) {
            auto* invoke_method = invoke_insn->get_method();
            callee = resolve_virtual(receiver_cls, invoke_method->get_name(),
                                     invoke_method->get_proto());
          }
        }
      }
      if (callee == nullptr) {
        callee = resolve_invoke_method(invoke_insn, caller);
      }
      if (!consider_callee(callee)) {
        return nullptr;
      }
      caller = callee;
      invoke_insn = nullptr;
    } while (is_simple(callee, &invoke_insn) && invoke_insn != nullptr);
    return callee;
  };

  // Analyze caller/callee relationships
  std::atomic<size_t> callers_too_many_instructions{0};
  std::atomic<size_t> callers_too_many_registers{0};
  InsertOnlyConcurrentSet<DexMethod*> hot_cold_callees;
  InsertOnlyConcurrentSet<DexMethod*> hot_hot_callees;
  InsertOnlyConcurrentMap<DexMethod*, uint32_t> estimated_code_units;
  InsertOnlyConcurrentMap<DexMethod*, size_t> estimated_instructions;
  InsertOnlyConcurrentMap<DexMethod*, bool> has_catches;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    uint32_t ecu = code.estimate_code_units();
    estimated_code_units.emplace(method, ecu);
    size_t instructions = code.count_opcodes();
    estimated_instructions.emplace(method, instructions);

    auto blocks = code.cfg().blocks();
    bool has_catch =
        std::any_of(blocks.begin(), blocks.end(),
                    [](cfg::Block* block) { return block->is_catch(); });
    has_catches.emplace(method, has_catch);

    type_inference::TypeInference ti(code.cfg());
    ti.run(method);
    const auto& type_envs = ti.get_type_environments();
    ReceiverMap map;
    for (auto& mie : InstructionIterable(code.cfg())) {
      auto* insn = mie.insn;
      auto op = insn->opcode();
      if (!opcode::is_invoke_virtual(op) && !opcode::is_invoke_interface(op)) {
        continue;
      }
      always_assert(type_envs.count(insn));
      const auto& env = type_envs.at(insn);
      auto dex_type = env.get_dex_type(insn->src(0));
      if (dex_type.has_value()) {
        map.emplace(insn, *dex_type);
      }
    }
    receiver_types.emplace(method, std::move(map));
  });
  auto method_profile_appears = [&](DexMethod* method, float appear_threshold) {
    for (const auto& p : method_profiles.all_interactions()) {
      auto it = p.second.find(method);
      if (it != p.second.end() &&
          it->second.appear_percent >= appear_threshold) {
        return true;
      }
    }
    return false;
  };
  auto is_sufficiently_hot = [&](cfg::Block* block, DexMethod* callee) {
    if (!is_compiled(baseline_profile, callee)) {
      return false;
    }
    if (hot_block_appear_threshold >= 0 &&
        !source_blocks::maybe_hot(block, hot_block_appear_threshold)) {
      return false;
    }
    if (hot_method_appear_threshold >= 0 &&
        !source_blocks::method_maybe_hot(callee, hot_method_appear_threshold) &&
        !method_profile_appears(callee, hot_method_appear_threshold)) {
      return false;
    }
    return true;
  };
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode& code) {
    if (!is_compiled(baseline_profile, caller)) {
      return;
    }
    size_t caller_instructions = estimated_instructions.at(caller);
    // Over the 1024 threshold of the AOT compiler, to be conservative.
    size_t MAX_INSTRUCTIONS = 1100;
    if (caller_instructions > MAX_INSTRUCTIONS) {
      callers_too_many_instructions.fetch_add(1);
      return;
    }
    size_t caller_registers = code.cfg().get_registers_size();
    size_t MAX_REGISTERS = 32;
    if (caller_registers > MAX_REGISTERS) {
      callers_too_many_registers.fetch_add(1);
      return;
    }
    for (auto* b : code.cfg().blocks()) {
      bool callsite_has_catch =
          code.cfg().get_succ_edge_of_type(b, cfg::EDGE_THROW) != nullptr;
      bool has_throw = false;
      bool has_non_init_invoke = false;
      for (auto rit = b->rbegin(); rit != b->rend(); ++rit) {
        if (rit->type != MFLOW_OPCODE) {
          continue;
        }
        auto* insn = rit->insn;
        if (!opcode::is_an_invoke(insn->opcode())) {
          if (opcode::is_throw(insn->opcode())) {
            has_throw = true;
          }
          continue;
        }
        if (has_throw && !has_non_init_invoke) {
          if (!method::is_init(insn->get_method())) {
            has_non_init_invoke = true;
          }
          continue;
        }

        DexMethod* callee = get_callee(caller, insn);
        if (callee == nullptr) {
          continue;
        }

        auto it = estimated_instructions.find(callee);
        if (it == estimated_instructions.end()) {
          continue;
        }

        if (callsite_has_catch && has_catches.at(callee)) {
          continue;
        }

        if (is_sufficiently_hot(b, callee)) {
          hot_hot_callees.insert(callee);
        } else {
          hot_cold_callees.insert(callee);
        }
      }
    }
  });
  mgr.incr_metric("never_inline_callers_too_many_instructions",
                  callers_too_many_instructions.load());
  mgr.incr_metric("never_inline_callers_too_many_registers",
                  callers_too_many_registers.load());
  mgr.incr_metric("never_inline_hot_cold_callees", hot_cold_callees.size());
  mgr.incr_metric("never_inline_hot_hot_callees", hot_hot_callees.size());

  // Attach annotation to callees where beneficial.
  std::atomic<size_t> callees_already_never_inline = 0;
  std::atomic<size_t> callees_too_hot = 0;
  std::atomic<size_t> callees_simple = 0;
  std::atomic<size_t> callees_too_small = 0;
  std::atomic<size_t> callees_too_large = 0;
  std::atomic<size_t> callees_always_throw = 0;
  std::atomic<size_t> callees_annotation_attached = 0;
  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    if (has_anno(method, type::dalvik_annotation_optimization_NeverInline())) {
      callees_already_never_inline.fetch_add(1);
      return;
    }

    if (hot_cold_callees.count_unsafe(method) == 0u) {
      return;
    }

    if (hot_hot_callees.count(method) != 0u) {
      callees_too_hot.fetch_add(1);
      return;
    }

    if (code.cfg().return_blocks().empty()) {
      callees_always_throw.fetch_add(1);
      return;
    }

    auto ecu = estimated_code_units.at(method);
    if (ecu > 40) {
      // Way over the 14 threshold of the AOT compiler, to be conservative.
      callees_too_large.fetch_add(1);
      return;
    }

    auto instructions = estimated_instructions.at(method);
    if (instructions <= 3) {
      callees_too_small.fetch_add(1);
      return;
    }

    if (is_simple(method)) {
      callees_simple.fetch_add(1);
      return;
    }

    callees_annotation_attached.fetch_add(1);
    if (!attach_annotations) {
      return;
    }
    if (method->get_anno_set() != nullptr) {
      method->get_anno_set()->combine_with(anno_set);
      return;
    }
    auto access = method->get_access();
    // attach_annotation_set requires the method to be synthetic.
    // A bit bizarre, and suggests that Redex' code to mutate annotations is
    // ripe for an overhaul. But I won't fight that here.
    method->set_access(access | ACC_SYNTHETIC);
    auto res = method->attach_annotation_set(
        std::make_unique<DexAnnotationSet>(anno_set));
    always_assert(res);
    method->set_access(access);
  });
  mgr.incr_metric("never_inline_callees_already_never_inline",
                  callees_already_never_inline.load());
  mgr.incr_metric("never_inline_callees_too_hot", callees_too_hot.load());
  mgr.incr_metric("never_inline_callees_simple", callees_simple.load());
  mgr.incr_metric("never_inline_callees_too_small", callees_too_small.load());
  mgr.incr_metric("never_inline_callees_too_large", callees_too_large.load());
  mgr.incr_metric("never_inline_callees_always_throw",
                  callees_always_throw.load());
  mgr.incr_metric("never_inline_callees_annotation_attached",
                  callees_annotation_attached.load());
}

bool never_compile_callcount_threshold_met(
    double call_count, int64_t never_compile_callcount_threshold) {
  return never_compile_callcount_threshold > -1 &&
         call_count <= never_compile_callcount_threshold;
}

bool never_compile_perf_threshold_met(DexMethod* method,
                                      int64_t never_compile_perf_threshold) {
  if (never_compile_perf_threshold <= -1) {
    return false;
  }

  int64_t sparse_switch_cases = 0;
  int64_t interpretation_cost = 20; // overhead of going into/out of interpreter
  for (auto* block : method->get_code()->cfg().blocks()) {
    if (is_cold(block)) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (opcode::is_an_internal(insn->opcode())) {
        continue;
      }
      // The interpreter uses expensive helper routines for a number of
      // instructions, which lead to an update of the hotness for JIT
      // purposes:
      // https://android.googlesource.com/platform/art/+/refs/heads/main/runtime/interpreter/mterp/nterp.cc
      // We assume that those instructions are more expensive than others
      // by an order of magnitude.
      interpretation_cost +=
          (insn->has_field() || insn->has_method() || insn->has_type() ||
           insn->has_string() || opcode::is_a_new(insn->opcode()))
              ? 10
              : 1;
      if (opcode::is_switch(insn->opcode()) && is_sparse(block)) {
        sparse_switch_cases += block->succs().size();
      }
    }
  }

  if (sparse_switch_cases == 0) {
    return false;
  }

  // We want to compare
  //    interpretation_cost / sparse_switch_cases
  //                            (the average cost of code per switch case)
  // with
  //    never_compile_perf_threshold * sparse_switch_cases
  //                                     (cost of executing sparse switch)
  // to find a case where the cost of the executing sparse switch
  // excessively dominates the cost of code per switch case, which the
  // following achieves.
  if ((interpretation_cost / std::pow(sparse_switch_cases, 2)) * 100 >
      never_compile_perf_threshold) {
    return false;
  }

  TRACE(APW, 5, "[%s] is within perf threshold: (%d / sqr(%d)) * 100 > %d\n%s",
        method->get_fully_deobfuscated_name().c_str(),
        (int32_t)interpretation_cost, (int32_t)sparse_switch_cases,
        (int32_t)never_compile_perf_threshold, SHOW(method->get_code()->cfg()));
  return true;
}

bool never_compile_called_coverage_threshold_met(
    DexMethod* method,
    double call_count,
    int64_t never_compile_called_coverage_threshold) {
  if (never_compile_called_coverage_threshold <= -1) {
    return false;
  }

  uint32_t covered_code_units = 0;
  uint32_t total_code_units = 0;
  for (auto* block : method->get_code()->cfg().blocks()) {
    auto ecu = block->estimate_code_units();
    total_code_units += ecu;
    if (!is_cold(block)) {
      covered_code_units += ecu;
    }
  }
  always_assert(total_code_units > 0);
  if (total_code_units < 24) {
    // Don't bother with small methods; adding annotation also creates
    // overhead. The chosen value is a bit larger than the 14-code units
    // inlining threshold of the AOT compiler.
    return false;
  }
  auto effective_call_count = std::max(1.0, call_count);
  if (effective_call_count * covered_code_units * 100 / total_code_units >=
      never_compile_called_coverage_threshold) {
    return false;
  }
  TRACE(APW, 5,
        "[%s] is within coverage threshold: %f %u / %u > %d percent\n%s",
        method->get_fully_deobfuscated_name().c_str(), effective_call_count,
        covered_code_units, total_code_units,
        (int32_t)never_compile_called_coverage_threshold,
        SHOW(method->get_code()->cfg()));
  return true;
}

bool never_compile_string_lookup_method_matches(
    DexMethod* method, bool never_compile_strings_lookup_methods) {
  if (!never_compile_strings_lookup_methods) {
    return false;
  }
  auto* cls = type_class(method->get_class());
  if ((cls == nullptr) || !cls->rstate.is_generated() ||
      cls->get_perf_sensitive() != PerfSensitiveGroup::STRINGS_LOOKUP) {
    return false;
  }
  TRACE(APW, 5, "[%s] matches string-lookup method",
        method->get_fully_deobfuscated_name().c_str());
  return true;
}

void never_compile_impl(
    const Scope& scope,
    const std::string& baseline_profile_name,
    const baseline_profiles::BaselineProfileConfig& baseline_profile_config,
    const method_profiles::MethodProfiles& method_profiles,
    PassManager& mgr,
    baseline_profiles::BaselineProfile* baseline_profile) {
  UnorderedSet<std::string> excluded_interaction_ids;
  const auto& harvest_config = baseline_profile_config.harvest_config;
  if (!harvest_config.never_compile_excluded_interaction_pattern.empty()) {
    boost::regex rx(harvest_config.never_compile_excluded_interaction_pattern);
    for (auto&& [interaction_id, interaction_name] :
         baseline_profile_config.interactions) {
      if (boost::regex_match(interaction_name, rx)) {
        excluded_interaction_ids.insert(interaction_id);
      }
    }
  }

  InsertOnlyConcurrentMap<DexMethod*, uint32_t> never_compile_methods;
  std::atomic<size_t> methods_already_never_compile = 0;
  std::atomic<size_t> methods_annotation_attached = 0;
  std::atomic<size_t> methods_annotation_not_attached = 0;
  std::atomic<size_t> never_compile_callcount_threshold_mets{0};
  std::atomic<size_t> never_compile_perf_threshold_mets{0};
  std::atomic<size_t> never_compile_called_coverage_threshold_mets{0};
  std::atomic<size_t> never_compile_strings_lookup_methods_matches{0};
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method::is_clinit(method)) {
      return;
    }
    auto it = baseline_profile->methods.find(method);
    if (!harvest_config.never_compile_ignore_hot) {
      if (it == baseline_profile->methods.end()) {
        return;
      }
      auto& mf = it->second;
      if (!mf.hot) {
        return;
      }
    }
    double call_count = 0;
    for (auto&& [interaction_id, _] : baseline_profile_config.interactions) {
      auto method_stats =
          method_profiles.get_method_stat(interaction_id, method);
      if (!method_stats) {
        continue;
      }
      if ((excluded_interaction_ids.count(interaction_id) != 0u) &&
          method_stats->appear_percent >
              harvest_config.never_compile_excluded_appear100_threshold &&
          method_stats->call_count >
              harvest_config.never_compile_excluded_call_count_threshold) {
        return;
      }
      call_count = std::max(call_count, method_stats->call_count);
    }

    loop_impl::LoopInfo loop_info(code.cfg());
    if (loop_info.num_loops() > 0) {
      return;
    }

    bool selected = false;
    if (never_compile_callcount_threshold_met(
            call_count, harvest_config.never_compile_callcount_threshold)) {
      never_compile_callcount_threshold_mets.fetch_add(1);
      selected = true;
    }

    if (never_compile_perf_threshold_met(
            method, harvest_config.never_compile_perf_threshold)) {
      never_compile_perf_threshold_mets.fetch_add(1);
      selected = true;
    }

    if (never_compile_called_coverage_threshold_met(
            method, call_count,
            harvest_config.never_compile_called_coverage_threshold)) {
      never_compile_called_coverage_threshold_mets.fetch_add(1);
      selected = true;
    }

    if (never_compile_string_lookup_method_matches(
            method, harvest_config.never_compile_strings_lookup_methods)) {
      never_compile_strings_lookup_methods_matches.fetch_add(1);
      selected = true;
    }

    if (!selected) {
      return;
    }

    never_compile_methods.emplace(method,
                                  method->get_code()->estimate_code_units());

    if (has_anno(method, type::dalvik_annotation_optimization_NeverCompile())) {
      methods_already_never_compile.fetch_add(1);
      return;
    }

    methods_annotation_not_attached.fetch_add(1);
    return;
  });
  for (auto&& [method, _0] : UnorderedIterable(never_compile_methods)) {
    baseline_profile->methods.erase(method);
  }
  mgr.incr_metric(baseline_profile_name + "_never_compile_methods",
                  never_compile_methods.size());
  mgr.incr_metric(baseline_profile_name + "_methods_already_never_compile",
                  methods_already_never_compile.load());
  mgr.incr_metric(baseline_profile_name + "_methods_annotation_attached",
                  methods_annotation_attached.load());
  mgr.incr_metric(baseline_profile_name + "_methods_annotation_not_attached",
                  methods_annotation_not_attached.load());
  mgr.incr_metric(baseline_profile_name + "_never_compile_callcount",
                  never_compile_callcount_threshold_mets.load());
  mgr.incr_metric(baseline_profile_name + "_never_compile_perf",
                  never_compile_perf_threshold_mets.load());
  mgr.incr_metric(baseline_profile_name + "_never_compile_called_coverage",
                  never_compile_called_coverage_threshold_mets.load());
  mgr.incr_metric(
      baseline_profile_name + "_never_compile_strings_lookup_methods_matches",
      never_compile_strings_lookup_methods_matches.load());
  auto ordered_never_compile_methods = unordered_to_ordered(
      never_compile_methods, [](const auto& a, const auto& b) {
        if (a.second != b.second) {
          return a.second > b.second;
        }
        return compare_dexmethods(a.first, b.first);
      });
  ordered_never_compile_methods.resize(
      std::min(ordered_never_compile_methods.size(), size_t(10)));
  for (size_t i = 0; i < ordered_never_compile_methods.size(); ++i) {
    auto& [method, code_units] = ordered_never_compile_methods[i];
    mgr.incr_metric(baseline_profile_name + "_never_compile_methods_" +
                        std::to_string(i) + "_" + show_deobfuscated(method),
                    code_units);
  }
}

void never_compile(
    const Scope& scope,
    const baseline_profiles::BaselineProfileConfigMap& baseline_profile_configs,
    const method_profiles::MethodProfiles& method_profiles,
    PassManager& mgr,
    UnorderedMap<std::string, baseline_profiles::BaselineProfile>&
        baseline_profiles) {
  for (auto& entry : UnorderedIterable(baseline_profiles)) {
    const auto& bp_name = entry.first;
    const auto& cur_bp_config = baseline_profile_configs.at(bp_name);
    if (!cur_bp_config.harvest_config.enable_never_compile) {
      continue;
    }
    auto& bp = entry.second;
    never_compile_impl(scope, bp_name, cur_bp_config, method_profiles, mgr,
                       &bp);
  }
}

void write_classes(const baseline_profiles::BaselineProfile& bp,
                   std::ostream& os) {
  std::vector<std::string_view> class_names;
  size_t deob_issues{0};
  unordered_for_each(bp.classes, [&](auto* cls) {
    if (cls->is_external()) {
      return;
    }
    auto* deobf_str = cls->get_deobfuscated_name_or_null();
    if (deobf_str == nullptr) {
      ++deob_issues;
      TRACE(APW, 1, "Failed to retrieve deobfuscated name for %s", SHOW(cls));
      deobf_str = cls->get_name();
    }
    redex_assert(deobf_str != nullptr);
    class_names.emplace_back(deobf_str->str());
  });
  if (deob_issues > 0) {
    TRACE(APW, 0,
          "Failed to retrieve deobfuscated name for %zu classes, run with "
          "TRACE=APW:1 to get details.",
          deob_issues);
  }

  os << "# " << class_names.size()
     << " classes from resolved and internal types\n";
  std::sort(class_names.begin(), class_names.end());
  for (auto& cls : class_names) {
    os << cls << "\n";
  }

  os << "# " << bp.unmatched_classes.size()
     << " classes from unresolved types\n";
  std::vector<std::string_view> unmatched;
  unordered_transform(bp.unmatched_classes, std::back_inserter(unmatched),
                      [](const auto& str) { return std::string_view(str); });
  std::sort(unmatched.begin(), unmatched.end());
  for (auto& cls : unmatched) {
    os << cls << "\n";
  }
}

} // namespace

std::ostream& operator<<(std::ostream& os,
                         const baseline_profiles::MethodFlags& flags) {
  if (flags.hot) {
    os << "H";
  }
  if (flags.startup) {
    os << "S";
  }
  if (flags.post_startup) {
    os << "P";
  }
  return os;
}

void ArtProfileWriterPass::bind_config() {
  bind("perf_appear100_threshold", m_perf_config.appear100_threshold,
       m_perf_config.appear100_threshold);
  bind("perf_call_count_threshold", m_perf_config.call_count_threshold,
       m_perf_config.call_count_threshold);
  bind("perf_coldstart_appear100_threshold",
       m_perf_config.coldstart_appear100_threshold,
       m_perf_config.coldstart_appear100_threshold);
  bind("perf_coldstart_appear100_nonhot_threshold",
       m_perf_config.coldstart_appear100_threshold,
       m_perf_config.coldstart_appear100_nonhot_threshold);
  bind("perf_interactions", m_perf_config.interactions,
       m_perf_config.interactions);
  bind("never_inline_estimate", false, m_never_inline_estimate);
  bind("never_inline_attach_annotations", false,
       m_never_inline_attach_annotations);
  bind("never_inline_hot_block_appear_threshold", -1.0f,
       m_never_inline_hot_block_appear_threshold);
  bind("never_inline_hot_method_appear_threshold", -1.0f,
       m_never_inline_hot_method_appear_threshold);

  bind("include_strings_lookup_class", false, m_include_strings_lookup_class);
  bind("override_strip_classes", std::nullopt, m_override_strip_classes,
       "Override the strip_classes flag to the one given.");
  after_configuration([this] {
    always_assert(m_perf_config.coldstart_appear100_nonhot_threshold <=
                  m_perf_config.coldstart_appear100_threshold);
  });
}

void ArtProfileWriterPass::eval_pass(DexStoresVector& /*stores*/,
                                     ConfigFiles& /*conf*/,
                                     PassManager& mgr) {
  if (m_never_inline_attach_annotations) {
    m_reserved_refs_handle = mgr.reserve_refs(name(),
                                              ReserveRefsInfo(/* frefs */ 0,
                                                              /* trefs */ 1,
                                                              /* mrefs */ 0));
  }
}

void write_methods(const Scope& scope,
                   const baseline_profiles::BaselineProfile& baseline_profile,
                   std::ofstream& ofs) {
  // We order H before not-H. In each category, we order SP -> S -> P -> none.
  struct MethodFlagsLess {
    bool operator()(const baseline_profiles::MethodFlags& lhs,
                    const baseline_profiles::MethodFlags& rhs) const {
      if (lhs.hot != rhs.hot) {
        return lhs.hot;
      }
      auto idx = [](const baseline_profiles::MethodFlags& flags) {
        return (flags.startup ? 2 : 0) + (flags.post_startup ? 1 : 0);
      };
      return idx(lhs) > idx(rhs);
    }
  };
  std::map<baseline_profiles::MethodFlags, std::vector<std::string>,
           MethodFlagsLess>
      methods;

  walk::classes(scope, [&](DexClass* cls) {
    for (auto* method : cls->get_all_methods()) {
      auto it = baseline_profile.methods.find(method);
      if (it == baseline_profile.methods.end()) {
        continue;
      }
      std::string descriptor = show_deobfuscated(method);
      // reformat it into manual profile pattern so baseline profile
      // generator in post-process can recognize the method
      boost::replace_all(descriptor, ".", "->");
      boost::replace_all(descriptor, ":(", "(");
      methods[it->second].emplace_back(std::move(descriptor));
    }
  });

  for (auto& p : methods) {
    if (!p.second.empty()) {
      ofs << "# " << p.second.size() << " " << p.first
          << " methods from write_methods().\n";
      std::sort(p.second.begin(), p.second.end());
      for (auto& str : p.second) {
        ofs << p.first << str << "\n";
      }
    }
  }
}

void ArtProfileWriterPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  if (m_never_inline_attach_annotations) {
    always_assert(m_reserved_refs_handle);
    mgr.release_reserved_refs(*m_reserved_refs_handle);
    m_reserved_refs_handle = std::nullopt;
  }

  UnorderedSet<const DexMethodRef*> method_refs_without_def;
  const auto& method_profiles = conf.get_method_profiles();

  auto scope = build_class_scope(stores);

  auto baseline_profiles_tuple = baseline_profiles::get_baseline_profiles(
      scope,
      conf.get_baseline_profile_configs(),
      method_profiles,
      &method_refs_without_def);
  auto& baseline_profiles = std::get<1>(baseline_profiles_tuple);
  auto& manual_profile = std::get<0>(baseline_profiles_tuple);

  // Load classes from input.
  manual_profile.load_classes(scope, conf, "manual");
  manual_profile.mark = manual_profile.classes.size();
  for (const auto& [config_name, baseline_profile_config] :
       UnorderedIterable(conf.get_baseline_profile_configs())) {
    auto& baseline_profile = baseline_profiles[config_name];
    baseline_profile.load_classes(scope, conf, config_name);
    baseline_profile.mark = baseline_profile.classes.size();
  }

  auto add_class = [&](DexClass* cls) {
    manual_profile.classes.insert(cls);
    for (const auto& [config_name, baseline_profile_config] :
         UnorderedIterable(conf.get_baseline_profile_configs())) {
      if (baseline_profile_config.options.use_final_redex_generated_profile) {
        auto& baseline_profile = baseline_profiles[config_name];
        baseline_profile.classes.insert(cls);
      }
    }
  };

  for (const auto& [config_name, baseline_profile_config] :
       UnorderedIterable(conf.get_baseline_profile_configs())) {

    if (baseline_profile_config.options.include_all_startup_classes) {
      const std::vector<std::string>& interdexorder =
          conf.get_coldstart_classes();
      std::vector<DexClass*> coldstart_classes;
      auto& baseline_profile = baseline_profiles[config_name];
      for (const auto& entry : interdexorder) {
        // Limit to just the 20% cold start set
        if (entry.find("ColdStart20PctEnd") != std::string::npos) {
          break;
        }

        DexType* type = DexType::get_type(entry);
        if (type != nullptr) {
          auto* coldstart_class = type_class(type);
          if (coldstart_class != nullptr) {
            coldstart_classes.push_back(coldstart_class);
            baseline_profile.classes.insert(coldstart_class);
          }
        }
      }

      baseline_profiles::MethodFlags flags;
      flags.hot = true;
      flags.startup = false;
      walk::methods(coldstart_classes,
                    [&baseline_profile, flags](DexMethod* method) {
                      baseline_profile.methods.emplace(method, flags);
                    });
    }
  }

  never_compile(scope, conf.get_baseline_profile_configs(), method_profiles,
                mgr, baseline_profiles);
  auto* store_fence_helper_type = DexType::get_type(STORE_FENCE_HELPER_NAME);
  if (store_fence_helper_type != nullptr) {
    // helper class existing means we materialized IOPCODE_WRITE_BARRIER
    // Add it in for it to be compiled.
    auto* store_fence_helper_cls = type_class(store_fence_helper_type);
    always_assert(store_fence_helper_cls);
    add_class(store_fence_helper_cls);
  }
  if (m_include_strings_lookup_class) {
    walk::classes(scope, [&](DexClass* cls) {
      if (cls->rstate.is_generated() &&
          cls->get_perf_sensitive() == PerfSensitiveGroup::STRINGS_LOOKUP) {
        add_class(cls);
      }
    });
  }

  auto resolve_strip_classes = [&](const auto& bp) {
    return m_override_strip_classes ? *m_override_strip_classes
                                    : bp.options.strip_classes;
  };

  // Transitively close classes. We always do this for metrics.
  if (conf.get_default_baseline_profile_config()
          .options.transitively_close_classes) {
    manual_profile.transitively_close_classes(scope);
  }
  for (auto& entry : UnorderedIterable(baseline_profiles)) {
    const auto& bp_name = entry.first;
    auto& bp = entry.second;
    const auto transitively_close_classes =
        conf.get_baseline_profile_configs()
            .at(bp_name)
            .options.transitively_close_classes;
    if (transitively_close_classes) {
      bp.transitively_close_classes(scope);
    }
  }

  for (const auto& entry : UnorderedIterable(baseline_profiles)) {
    const auto& bp_name = entry.first;
    const auto& bp = entry.second;
    const auto strip_classes =
        resolve_strip_classes(conf.get_baseline_profile_configs().at(bp_name));
    auto output_name = conf.metafile(bp_name + "-baseline-profile.txt");
    std::ofstream ofs{output_name.c_str()};
    if (!strip_classes) {
      write_classes(bp, ofs);
    }
    write_methods(scope, bp, ofs);
  }
  std::ofstream ofs{conf.metafile(BASELINE_PROFILES_FILE)};
  if (!resolve_strip_classes(conf.get_default_baseline_profile_config())) {
    write_classes(manual_profile, ofs);
  }
  write_methods(scope, manual_profile, ofs);

  auto gather_metrics = [&](const auto& bp_name, const auto& bp_config_name,
                            const auto& profile) {
    static const size_t APPEAR100_BUCKETS = 10;
    std::atomic<size_t> code_units{0};
    std::atomic<size_t> compiled_methods{0};
    std::atomic<size_t> compiled_code_units_cold{0};
    std::array<std::atomic<size_t>, APPEAR100_BUCKETS + 1>
        compiled_code_units_hot_by_appear100{};
    std::atomic<size_t> compiled_code_units_hot_super{0};
    std::atomic<size_t> compiled_code_units_unknown{0};
    InsertOnlyConcurrentSet<DexType*> method_classes;
    walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
      auto it = profile.methods.find(method);
      if (it == profile.methods.end()) {
        return;
      }
      method_classes.insert(method->get_class());
      auto ecu = code.estimate_code_units();
      code_units += ecu;
      if (is_compiled(method, it->second)) {
        compiled_methods++;
        size_t cold = 0;
        std::array<size_t, APPEAR100_BUCKETS + 1> hot_by_appear100{0};
        size_t hot_super = 0;
        size_t unknown = 0;
        for (auto* block : code.cfg().blocks()) {
          ecu = block->estimate_code_units();
          auto* sb = source_blocks::get_first_source_block(block);
          if (sb != nullptr && !sb->foreach_val_early([](const auto& v) {
                return !v || v->val > 0;
              })) {
            cold += ecu;
          } else if (sb != nullptr && sb->foreach_val_early([](const auto& v) {
                       return v && v->val > 0;
                     })) {
            float max_appear100 = 0;
            sb->foreach_val([&](const auto& v) {
              if (v) {
                max_appear100 = std::max(max_appear100, v->appear100);
              }
            });
            if (max_appear100 > 100) {
              hot_super += ecu;
            } else {
              size_t bucket = (size_t)(max_appear100 * APPEAR100_BUCKETS / 100);
              hot_by_appear100[bucket] += ecu;
            }
          } else {
            unknown += ecu;
          }
        }
        if (cold) {
          compiled_code_units_cold += cold;
        }
        for (size_t bucket = 0; bucket <= APPEAR100_BUCKETS; ++bucket) {
          if (hot_by_appear100[bucket]) {
            compiled_code_units_hot_by_appear100[bucket] +=
                hot_by_appear100[bucket];
          }
        }
        if (hot_super) {
          compiled_code_units_hot_super += hot_super;
        }
        if (unknown) {
          compiled_code_units_unknown += unknown;
        }
      }
    });

    size_t root_dexes_with_methods = [&]() {
      size_t cnt = 0;
      redex_assert(stores[0].is_root_store());
      for (const auto& dex : stores[0].get_dexen()) {
        for (auto* cls : dex) {
          if (method_classes.count_unsafe(cls->get_type())) {
            ++cnt;
            break;
          }
        }
      }
      return cnt;
    }();

    auto prefix = std::string("profile_") + bp_name + "_";
    mgr.incr_metric(prefix + "classes.in", profile.mark);
    mgr.incr_metric(prefix + "classes", profile.classes.size());
    mgr.incr_metric(prefix + "unmatched_classes",
                    profile.unmatched_classes.size());
    mgr.incr_metric(prefix + "methods", profile.methods.size());
    mgr.incr_metric(prefix + "code_units", (size_t)code_units);
    mgr.incr_metric(prefix + "compiled", (size_t)compiled_methods);
    size_t compiled_code_units_hot = compiled_code_units_hot_super;
    for (size_t bucket = 0; bucket <= APPEAR100_BUCKETS; ++bucket) {
      auto appear100 = (size_t)compiled_code_units_hot_by_appear100[bucket];
      compiled_code_units_hot += appear100;
      std::stringstream ss;
      ss << std::setw(3) << std::setfill('0')
         << (bucket * (100 / APPEAR100_BUCKETS));
      mgr.incr_metric(prefix + "compiled_code_units_hot_appear" + ss.str(),
                      appear100);
    }
    mgr.incr_metric(prefix + "compiled_code_units",
                    (size_t)compiled_code_units_cold +
                        (size_t)compiled_code_units_hot +
                        (size_t)compiled_code_units_unknown);
    mgr.incr_metric(prefix + "compiled_code_units_cold",
                    (size_t)compiled_code_units_cold);
    mgr.incr_metric(prefix + "compiled_code_units_hot",
                    (size_t)compiled_code_units_hot);
    mgr.incr_metric(prefix + "compiled_code_units_hot_super",
                    (size_t)compiled_code_units_hot_super);
    mgr.incr_metric(prefix + "compiled_code_units_unknown",
                    (size_t)compiled_code_units_unknown);

    const auto& bp_config =
        conf.get_baseline_profile_configs().at(bp_config_name);
    for (auto&& [interaction_id, interaction_name] : bp_config.interactions) {
      mgr.incr_metric(prefix + "interaction_" + interaction_id, 1);
    }
    const auto& bp_options = bp_config.options;
    if (bp_options.oxygen_modules) {
      mgr.incr_metric(prefix + "oxygen_modules", 1);
    }
    if (bp_options.strip_classes &&
        (!m_override_strip_classes || *m_override_strip_classes)) {
      mgr.incr_metric(prefix + "strip_classes", 1);
    }
    if (bp_options.transitively_close_classes) {
      mgr.incr_metric(prefix + "transitively_close_classes", 1);
    }
    if (bp_options.use_redex_generated_profile) {
      mgr.incr_metric(prefix + "use_redex_generated_profile", 1);
    }
    if (bp_options.include_all_startup_classes) {
      mgr.incr_metric(prefix + "include_all_startup_classes", 1);
    }
    if (bp_options.use_final_redex_generated_profile) {
      mgr.incr_metric(prefix + "use_final_redex_generated_profile", 1);
    }

    mgr.incr_metric(prefix + "root_dexes_with_profile_methods",
                    root_dexes_with_methods);
  };
  gather_metrics("manual",
                 baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME,
                 manual_profile);
  for (auto&& [name, profile] : UnorderedIterable(baseline_profiles)) {
    always_assert(name != "manual");
    gather_metrics(name, name, profile);
  }

  mgr.incr_metric("method_refs_without_def", method_refs_without_def.size());

  mgr.incr_metric(
      "used_bzl_baseline_profile_config",
      static_cast<int64_t>(conf.get_did_use_bzl_baseline_profile_config()));

  InsertOnlyConcurrentMap<DexMethod*, uint32_t> huge_methods;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    auto code_units = code.estimate_code_units();
    code_units += code.cfg().get_size_adjustment();
    if (code_units > assessments::HUGE_METHOD_THRESHOLD) {
      huge_methods.emplace(method, code_units);
    }
  });
  for (auto [method, code_units] : UnorderedIterable(huge_methods)) {
    mgr.incr_metric("huge_methods_" + show_deobfuscated(method), code_units);
  }

  if (!m_never_inline_estimate && !m_never_inline_attach_annotations) {
    return;
  }

  never_inline(m_never_inline_attach_annotations,
               m_never_inline_hot_block_appear_threshold,
               m_never_inline_hot_method_appear_threshold, scope,
               manual_profile, mgr, method_profiles);
}

static ArtProfileWriterPass s_pass;
