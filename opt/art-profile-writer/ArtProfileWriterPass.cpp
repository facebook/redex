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
#include "DexStructure.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "LoopInfo.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "SourceBlocks.h"
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
    if (invoke_insn) {
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
                  const Scope& scope,
                  const baseline_profiles::BaselineProfile& baseline_profile,
                  PassManager& mgr) {
  DexAnnotationSet anno_set;
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      type::dalvik_annotation_optimization_NeverInline(),
      DexAnnotationVisibility::DAV_BUILD));

  auto consider_callee = [&](DexMethod* callee) {
    if (callee == nullptr || !callee->get_code()) {
      return false;
    }
    auto* cls = type_class(callee->get_class());
    if (!cls || cls->is_external()) {
      return false;
    }
    if (callee->is_virtual() && (!is_final(callee) && !is_final(cls))) {
      return false;
    }
    return true;
  };

  auto get_callee = [&](DexMethod* caller,
                        IRInstruction* invoke_insn) -> DexMethod* {
    DexMethod* callee;
    do {
      callee = resolve_invoke_method(invoke_insn, caller);
      if (!consider_callee(callee)) {
        return nullptr;
      }
      caller = callee;
      invoke_insn = nullptr;
    } while (is_simple(callee, &invoke_insn) && invoke_insn != nullptr);
    return callee;
  };

  // Analyze caller/callee relationships
  std::atomic<size_t> callers_too_large{0};
  InsertOnlyConcurrentSet<DexMethod*> hot_cold_callees;
  InsertOnlyConcurrentSet<DexMethod*> hot_hot_callees;
  InsertOnlyConcurrentMap<DexMethod*, size_t> estimated_code_units;
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode& code) {
    auto ecu = code.estimate_code_units();
    estimated_code_units.emplace(caller, ecu);
    if (!is_compiled(baseline_profile, caller)) {
      return;
    }
    if (ecu > 2048) {
      // Way over the 1024 threshold of the AOT compiler, to be conservative.
      callers_too_large.fetch_add(1);
      return;
    }
    for (auto* b : code.cfg().blocks()) {
      for (auto& mie : InstructionIterable(b)) {
        if (!opcode::is_an_invoke(mie.insn->opcode())) {
          continue;
        }

        DexMethod* callee = get_callee(caller, mie.insn);
        if (!callee) {
          continue;
        }

        if (is_compiled(baseline_profile, callee)) {
          hot_hot_callees.insert(callee);
        } else {
          hot_cold_callees.insert(callee);
        }
      }
    }
  });
  mgr.incr_metric("never_inline_callers_too_large", callers_too_large.load());
  mgr.incr_metric("never_inline_hot_cold_callees", hot_cold_callees.size());
  mgr.incr_metric("never_inline_hot_hot_callees", hot_hot_callees.size());

  // Attach annotation to callees where beneficial.
  std::atomic<size_t> callees_already_never_inline = 0;
  std::atomic<size_t> callees_too_hot = 0;
  std::atomic<size_t> callees_simple = 0;
  std::atomic<size_t> callees_too_small = 0;
  std::atomic<size_t> callees_too_large = 0;
  std::atomic<size_t> callees_annotation_attached = 0;
  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    if (has_anno(method, type::dalvik_annotation_optimization_NeverInline())) {
      callees_already_never_inline.fetch_add(1);
      return;
    }

    if (!hot_cold_callees.count_unsafe(method)) {
      return;
    }

    if (hot_hot_callees.count(method)) {
      callees_too_hot.fetch_add(1);
      return;
    }

    auto ecu = code.estimate_code_units();
    if (ecu > 32) {
      // Way over the 14 threshold of the AOT compiler, to be conservative.
      callees_too_large.fetch_add(1);
      return;
    }

    if (ecu <= 3) {
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
    if (method->get_anno_set()) {
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
  mgr.incr_metric("never_inline_callees_annotation_attached",
                  callees_annotation_attached.load());
}

void never_compile(
    const Scope& scope,
    const std::unordered_map<std::string,
                             baseline_profiles::BaselineProfileConfig>&
        baseline_profile_configs,
    const method_profiles::MethodProfiles& method_profiles,
    const std::vector<std::string>& interactions,
    PassManager& mgr,
    int64_t never_compile_callcount_threshold,
    int64_t never_compile_perf_threshold,
    const std::string& excluded_interaction_pattern,
    int64_t excluded_appear100_threshold,
    int64_t excluded_call_count_threshold,
    std::unordered_map<std::string, baseline_profiles::BaselineProfile>*
        baseline_profiles) {
  for (const auto& [config_name, baseline_profile_config] :
       baseline_profile_configs) {
    auto baseline_profile = baseline_profiles->at(config_name);
    std::unordered_set<std::string> excluded_interaction_ids;
    if (!excluded_interaction_pattern.empty()) {
      boost::regex rx(excluded_interaction_pattern);
      for (auto&& [interaction_id, interaction_name] :
           baseline_profile_config.interactions) {
        if (boost::regex_match(interaction_name, rx)) {
          excluded_interaction_ids.insert(interaction_id);
        }
      }
    }

    DexAnnotationSet anno_set;
    anno_set.add_annotation(std::make_unique<DexAnnotation>(
        type::dalvik_annotation_optimization_NeverCompile(),
        DexAnnotationVisibility::DAV_BUILD));

    std::atomic<size_t> never_compile_methods = 0;
    std::atomic<size_t> methods_already_never_compile = 0;
    std::atomic<size_t> methods_annotation_attached = 0;
    walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
      if (method::is_clinit(method)) {
        return;
      }
      auto it = baseline_profile.methods.find(method);
      if (it == baseline_profile.methods.end()) {
        return;
      }
      auto& mf = it->second;
      if (!mf.hot) {
        return;
      }
      double call_count = 0;
      for (auto& interaction_id : interactions) {
        auto method_stats =
            method_profiles.get_method_stat(interaction_id, method);
        if (!method_stats) {
          continue;
        }
        if (excluded_interaction_ids.count(interaction_id) &&
            method_stats->appear_percent > excluded_appear100_threshold &&
            method_stats->call_count > excluded_call_count_threshold) {
          return;
        }
        call_count = std::max(call_count, method_stats->call_count);
      }
      if (never_compile_callcount_threshold > -1 &&
          call_count > never_compile_callcount_threshold) {
        return;
      }
      loop_impl::LoopInfo loop_info(code.cfg());
      if (loop_info.num_loops() > 0) {
        return;
      }

      if (never_compile_perf_threshold > -1) {
        int64_t sparse_switch_cases = 0;
        int64_t interpretation_cost =
            20; // overhead of going into/out of interpreter
        for (auto* block : code.cfg().blocks()) {
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
          return;
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
        if (interpretation_cost / std::pow(sparse_switch_cases, 2) >
            never_compile_perf_threshold) {
          return;
        }

        TRACE(APW, 5, "[%s] is within perf threshold: %d / sqr(%d) > %d\n%s",
              method->get_fully_deobfuscated_name().c_str(),
              (int32_t)interpretation_cost, (int32_t)sparse_switch_cases,
              (int32_t)never_compile_perf_threshold, SHOW(code.cfg()));
      }

      never_compile_methods.fetch_add(1);

      if (has_anno(method,
                   type::dalvik_annotation_optimization_NeverCompile())) {
        methods_already_never_compile.fetch_add(1);
        return;
      }

      methods_annotation_attached.fetch_add(1);
      if (method->get_anno_set()) {
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
      mf.hot = false;
    });
    mgr.incr_metric("never_compile_methods", never_compile_methods.load());
    mgr.incr_metric("methods_already_never_compile",
                    methods_already_never_compile.load());
    mgr.incr_metric("methods_annotation_attached",
                    methods_annotation_attached.load());
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
  bind("legacy_mode", true, m_legacy_mode);
  bind("never_compile_callcount_threshold", -1,
       m_never_compile_callcount_threshold);
  bind("never_compile_perf_threshold", -1, m_never_compile_perf_threshold);
  bind("never_compile_excluded_interaction_pattern", "",
       m_never_compile_excluded_interaction_pattern);
  bind("never_compile_excluded_appear100_threshold", 20,
       m_never_compile_excluded_appear100_threshold);
  bind("never_compile_excluded_call_count_threshold", 0,
       m_never_compile_excluded_call_count_threshold);
  after_configuration([this] {
    always_assert(m_perf_config.coldstart_appear100_nonhot_threshold <=
                  m_perf_config.coldstart_appear100_threshold);
  });
}

void ArtProfileWriterPass::eval_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
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
                   bool strip_classes,
                   std::ofstream& ofs) {
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
      ofs << it->second << descriptor << "\n";
      if (baseline_profile.classes.count(cls) && !strip_classes) {
        ofs << show_deobfuscated(cls) << "\n";
      }
    }
  });
}

void ArtProfileWriterPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  if (m_never_inline_attach_annotations) {
    always_assert(m_reserved_refs_handle);
    mgr.release_reserved_refs(*m_reserved_refs_handle);
    m_reserved_refs_handle = std::nullopt;
  }

  std::unordered_set<const DexMethodRef*> method_refs_without_def;
  const auto& method_profiles = conf.get_method_profiles();
  auto get_legacy_baseline_profile = [&]() {
    std::unordered_map<std::string, baseline_profiles::BaselineProfile>
        baseline_profiles;
    baseline_profiles::BaselineProfile res;
    for (auto& interaction_id : m_perf_config.interactions) {
      bool startup = interaction_id == "ColdStart";
      const auto& method_stats = method_profiles.method_stats(interaction_id);
      for (auto&& [method_ref, stat] : method_stats) {
        auto method = method_ref->as_def();
        if (method == nullptr) {
          method_refs_without_def.insert(method_ref);
          continue;
        }
        // for startup interaction, we can include it into baseline profile
        // as non hot method if the method appear100 is above nonhot_threshold
        if (stat.appear_percent >=
                (startup ? m_perf_config.coldstart_appear100_nonhot_threshold
                         : m_perf_config.appear100_threshold) &&
            stat.call_count >= m_perf_config.call_count_threshold) {
          auto& mf = res.methods[method];
          mf.hot = startup ? stat.appear_percent >=
                                 m_perf_config.coldstart_appear100_threshold
                           : true;
          if (startup) {
            // consistent with buck python config in the post-process baseline
            // profile generator, which is set both flags true for ColdStart
            // methods
            mf.startup = true;
            // if startup method is not hot, we do not set its post_startup flag
            // the method still has a change to get it set if it appears in
            // other interactions' hot list. Remember, ART only uses this flag
            // to guide dexlayout decision, so we don't have to be pedantic to
            // assume it never gets exectued post startup
            mf.post_startup = mf.hot;
          } else {
            mf.post_startup = true;
          }
        }
      }
    }
    auto& dexen = stores.front().get_dexen();
    int32_t min_sdk = mgr.get_redex_options().min_sdk;
    mgr.incr_metric("min_sdk", min_sdk);
    auto end = min_sdk >= 21 ? dexen.size() : 1;
    for (size_t dex_idx = 0; dex_idx < end; dex_idx++) {
      auto& dex = dexen.at(dex_idx);
      for (auto* cls : dex) {
        bool should_include_class = false;
        for (auto* method : cls->get_all_methods()) {
          auto it = res.methods.find(method);
          if (it == res.methods.end()) {
            continue;
          }
          // hot method's class should be included.
          // In addition, if we include non-hot startup method, we also need to
          // include its class.
          if (it->second.hot ||
              (it->second.startup && !it->second.post_startup)) {
            should_include_class = true;
          }
        }
        if (should_include_class) {
          res.classes.insert(cls);
        }
      }
    }
    baseline_profiles[baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME] =
        res;
    return baseline_profiles;
  };

  auto baseline_profiles = m_legacy_mode
                               ? get_legacy_baseline_profile()
                               : baseline_profiles::get_baseline_profiles(
                                     conf.get_baseline_profile_configs(),
                                     method_profiles,
                                     conf.get_json_config().get(
                                         "ingest_baseline_profile_data", false),
                                     &method_refs_without_def);
  for (const auto& [config_name, baseline_profile_config] :
       conf.get_baseline_profile_configs()) {
    if (baseline_profile_config.options.include_all_startup_classes) {
      const std::vector<std::string>& interdexorder =
          conf.get_coldstart_classes();
      std::vector<DexClass*> coldstart_classes;
      auto& baseline_profile = baseline_profiles[config_name];
      for (const auto& entry : interdexorder) {
        DexType* type = DexType::get_type(entry);
        if (type) {
          auto coldstart_class = type_class(type);
          if (coldstart_class) {
            coldstart_classes.push_back(coldstart_class);
            baseline_profile.classes.insert(coldstart_class);
          }
        }
      }
      baseline_profiles::MethodFlags flags;
      flags.hot = true;
      flags.startup = true;
      walk::methods(coldstart_classes,
                    [&baseline_profile, flags](DexMethod* method) {
                      baseline_profile.methods.emplace(method, flags);
                    });
    }
  }
  auto scope = build_class_scope(stores);
  if (m_never_compile_callcount_threshold > -1 ||
      m_never_compile_perf_threshold > -1) {
    never_compile(
        scope, conf.get_baseline_profile_configs(), method_profiles,
        m_perf_config.interactions, mgr, m_never_compile_callcount_threshold,
        m_never_compile_perf_threshold,
        m_never_compile_excluded_interaction_pattern,
        m_never_compile_excluded_appear100_threshold,
        m_never_compile_excluded_call_count_threshold, &baseline_profiles);
  }
  auto store_fence_helper_type = DexType::get_type(STORE_FENCE_HELPER_NAME);
  if (store_fence_helper_type) {
    // helper class existing means we materialized IOPCODE_WRITE_BARRIER
    // Add it in for it to be compiled.
    auto store_fence_helper_cls = type_class(store_fence_helper_type);
    always_assert(store_fence_helper_cls);
    for (auto it = baseline_profiles.begin(); it != baseline_profiles.end();
         it++) {
      it->second.classes.insert(store_fence_helper_cls);
    }
  }

  if (conf.get_json_config().get("ingest_baseline_profile_data", false)) {
    for (const auto& entry : baseline_profiles) {
      const auto& bp_name = entry.first;
      const auto& bp = entry.second;
      const auto strip_classes =
          conf.get_baseline_profile_configs().at(bp_name).options.strip_classes;
      auto preprocessed_profile_name =
          conf.get_preprocessed_baseline_profile_file(bp_name);
      auto output_name = conf.metafile(bp_name + "-baseline-profile.txt");
      std::ofstream ofs{output_name.c_str()};
      std::ifstream preprocessed_profile(preprocessed_profile_name);
      std::string current_line;
      if (!strip_classes) {
        while (std::getline(preprocessed_profile, current_line)) {
          if (current_line.empty() || current_line[0] != 'L') {
            continue;
          }
          ofs << current_line << "\n";
        }
      }
      write_methods(scope, bp, strip_classes, ofs);
    }
  } else {
    std::ofstream ofs{conf.metafile(BASELINE_PROFILES_FILE)};
    auto baseline_profile = baseline_profiles.at(
        baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME);
    const auto strip_classes =
        conf.get_default_baseline_profile_config().options.strip_classes;
    write_methods(scope, baseline_profile, strip_classes, ofs);
  }

  std::atomic<size_t> methods_with_baseline_profile_code_units{0};
  std::atomic<size_t> compiled{0};
  std::atomic<size_t> compiled_code_units{0};
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    for (auto profile_it = baseline_profiles.begin();
         profile_it != baseline_profiles.end();
         profile_it++) {
      auto it = profile_it->second.methods.find(method);
      if (it == profile_it->second.methods.end()) {
        return;
      }
      auto ecu = code.estimate_code_units();
      methods_with_baseline_profile_code_units += ecu;
      if (is_compiled(method, it->second)) {
        compiled++;
        compiled_code_units += ecu;
      }
    }
  });

  mgr.incr_metric(
      "classes_with_baseline_profile",
      baseline_profiles
          .at(baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME)
          .classes.size());

  mgr.incr_metric(
      "methods_with_baseline_profile",
      baseline_profiles
          .at(baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME)
          .methods.size());

  mgr.incr_metric("methods_with_baseline_profile_code_units",
                  (size_t)methods_with_baseline_profile_code_units);

  mgr.incr_metric("compiled", (size_t)compiled);

  mgr.incr_metric("compiled_code_units", (size_t)compiled_code_units);

  mgr.incr_metric("method_refs_without_def", method_refs_without_def.size());

  mgr.incr_metric("used_bzl_baseline_profile_config",
                  conf.get_did_use_bzl_baseline_profile_config());

  if (!m_never_inline_estimate && !m_never_inline_attach_annotations) {
    return;
  }

  never_inline(m_never_inline_attach_annotations, scope,
               baseline_profiles.at(
                   baseline_profiles::DEFAULT_BASELINE_PROFILE_CONFIG_NAME),
               mgr);
}

static ArtProfileWriterPass s_pass;
