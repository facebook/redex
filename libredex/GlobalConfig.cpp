/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalConfig.h"

void InlinerConfig::bind_config() {
  bind("true_virtual_inline", true_virtual_inline, true_virtual_inline);
  bind("use_cfg_inliner", use_cfg_inliner, use_cfg_inliner);
  bind("enforce_method_size_limit", enforce_method_size_limit,
       enforce_method_size_limit);
  bind("throws", throws_inline, throws_inline);
  bind("multiple_callers", multiple_callers, multiple_callers);
  bind("inline_small_non_deletables", inline_small_non_deletables,
       inline_small_non_deletables);
  bind("run_const_prop", run_const_prop, run_const_prop);
  bind("run_cse", run_cse, run_cse);
  bind("run_dedup_blocks", run_dedup_blocks, run_dedup_blocks);
  bind("run_copy_prop", run_copy_prop, run_copy_prop);
  bind("run_local_dce", run_local_dce, run_local_dce);
  bind("no_inline_annos", {}, m_no_inline_annos);
  bind("force_inline_annos", {}, m_force_inline_annos);
  bind("black_list", {}, m_black_list);
  bind("black_list", {}, m_black_list);
  bind("caller_black_list", {}, m_caller_black_list);
}

void OptDecisionsConfig::bind_config() {
  bind("enable_logs", false, enable_logs,
       "Should we log Redex's optimization decisions?");
}

void IRTypeCheckerConfig::bind_config() {
  bind("run_after_each_pass", {}, run_after_each_pass);
  bind("verify_moves", {}, verify_moves);
  bind("run_after_passes", {}, run_after_passes);
  bind("check_no_overwrite_this", {}, check_no_overwrite_this);
}

void HasherConfig::bind_config() {
  bind("run_after_each_pass", {}, run_after_each_pass);
}

void CheckUniqueDeobfuscatedNamesConfig::bind_config() {
  bind("run_after_each_pass", run_after_each_pass, run_after_each_pass);
  bind("run_initially", run_initially, run_initially);
  bind("run_finally", run_finally, run_finally);
}

void GlobalConfig::bind_config() {
  OptDecisionsConfig opt_decisions_param;
  IRTypeCheckerConfig ir_type_checker_param;
  HasherConfig hasher_param;
  CheckUniqueDeobfuscatedNamesConfig check_unique_deobfuscated_names_config;
  InlinerConfig inliner_param;
  bool bool_param;
  std::string string_param;
  std::vector<std::string> string_vector_param;
  uint32_t uint32_param;
  // Sorted alphabetically
  bind("agg_method_stats_files", {}, string_vector_param);
  bind("android_sdk_api_15_file", "", string_param);
  bind("android_sdk_api_16_file", "", string_param);
  bind("android_sdk_api_21_file", "", string_param);
  bind("android_sdk_api_23_file", "", string_param);
  bind("android_sdk_api_25_file", "", string_param);
  bind("android_sdk_api_26_file", "", string_param);
  bind("bytecode_sort_mode", {}, string_vector_param);
  bind("legacy_profiled_code_item_sort_order", true, bool_param);
  bind("coldstart_classes", "", string_param);
  bind("compute_xml_reachability", false, bool_param);
  bind("unused_keep_rule_abort", false, bool_param);
  bind("debug_info_kind", "", string_param);
  bind("default_coldstart_classes", "", string_param);
  bind("emit_class_method_info_map", false, bool_param);
  bind("emit_locator_strings", {}, bool_param);
  bind("force_single_dex", false, bool_param);
  bind("inliner", InlinerConfig(), inliner_param);
  bind("instruction_size_bitwidth_limit", 0u, uint32_param);
  bind("ir_type_checker", IRTypeCheckerConfig(), ir_type_checker_param);
  bind("hasher", HasherConfig(), hasher_param);
  bind("check_unique_deobfuscated_names", CheckUniqueDeobfuscatedNamesConfig(), check_unique_deobfuscated_names_config);
  bind("json_serde_supercls", {}, string_vector_param);
  bind("keep_all_annotation_classes", true, bool_param);
  bind("keep_methods", {}, string_vector_param);
  bind("keep_packages", {}, string_vector_param);
  bind("legacy_reflection_reachability", false, bool_param);
  bind("lower_with_cfg", {}, bool_param);
  bind("method_sorting_whitelisted_substrings", {}, string_vector_param);
  bind("no_optimizations_annotations", {}, string_vector_param);
  bind("opt_decisions", OptDecisionsConfig(), opt_decisions_param);
  bind("proguard_map", "", string_param);
  bind("prune_unexported_components", {}, string_vector_param);
  bind("pure_methods", {}, string_vector_param);
  bind("record_keep_reasons", {}, bool_param);
  bind("string_sort_mode", "", string_param);
  bind("write_cfg_each_pass", false, bool_param);
  bind("dump_cfg_classes", "", string_param);
}
