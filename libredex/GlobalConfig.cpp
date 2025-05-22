/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalConfig.h"

void InlinerConfig::bind_config() {
  bind("delete_non_virtuals", delete_non_virtuals, delete_non_virtuals);
  bind("true_virtual_inline", true_virtual_inline, true_virtual_inline);
  bind("relaxed_init_inline", relaxed_init_inline, relaxed_init_inline);
  bind("unfinalize_relaxed_init_inline", unfinalize_relaxed_init_inline,
       unfinalize_relaxed_init_inline);
  bind("unfinalize_perf_mode", "not-cold", unfinalize_perf_mode_str,
       "one of \"none\", \"not-cold\", \"maybe-hot\", \"hot\"");
  bind("strict_throwable_init_inline", strict_throwable_init_inline,
       strict_throwable_init_inline);
  bind("intermediate_shrinking", intermediate_shrinking,
       intermediate_shrinking);
  bind("enforce_method_size_limit", enforce_method_size_limit,
       enforce_method_size_limit);
  bind("throws", throws_inline, throws_inline);
  bind("throw_after_no_return", throw_after_no_return, throw_after_no_return);
  bind("max_cost_for_constant_propagation", max_cost_for_constant_propagation,
       max_cost_for_constant_propagation);
  bind("multiple_callers", multiple_callers, multiple_callers);
  bind("run_const_prop", shrinker.run_const_prop, shrinker.run_const_prop);
  bind("run_cse", shrinker.run_cse, shrinker.run_cse);
  bind("run_dedup_blocks", shrinker.run_dedup_blocks,
       shrinker.run_dedup_blocks);
  bind("run_branch_prefix_hoisting", shrinker.run_branch_prefix_hoisting,
       shrinker.run_branch_prefix_hoisting);
  bind("run_copy_prop", shrinker.run_copy_prop, shrinker.run_copy_prop);
  bind("run_reg_alloc", shrinker.run_reg_alloc, shrinker.run_reg_alloc);
  bind("run_fast_reg_alloc", shrinker.run_fast_reg_alloc,
       shrinker.run_fast_reg_alloc);
  bind("run_local_dce", shrinker.run_local_dce, shrinker.run_local_dce);
  bind("no_inline_annos", {}, no_inline_annos,
       "When any of these annotations is present on a method or class, then "
       "this method or all methods of this class will not get inlined at any "
       "callsite, and callsites will not get deduplicated.");
  bind("no_inline_blocklist", {}, no_inline_blocklist,
       "Any method matching any given prefix will not get inlined at any "
       "callsite, and callsites will not get deduplicated.");
  bind("force_inline_annos", {}, force_inline_annos,
       "When any of these annotations is present on a method or class, then "
       "this method or all methods of this class will get inlined at all "
       "callsites if possible.");
  bind("blocklist", {}, blocklist,
       "Any method defined in a class matching any given prefix will not get "
       "inlined at any callsite. This is problematic as Redex may move methods "
       "across classes. Avoid this annotation, prefer using "
       "no_inline_blocklist.");
  bind("caller_blocklist", {}, caller_blocklist,
       "Any method defined in a class matching any given prefix will not t "
       "inlined at all callsite if possible. This is problematic as Redex may "
       "move methods across classes.");
  bind("intradex_allowlist", {}, intradex_allowlist,
       "The purpose of this white-list is to remove black-list entries when "
       "inlining after the InterDex pass has run. (This reduces the impact of "
       "black-list entries that avoid inlining conditional control-flow and "
       "catchers that cause issues with the SwitchMethodPartitioning analysis "
       "that tends to be used by passes that run before or during InterDex.)");
}

void OptDecisionsConfig::bind_config() {
  bind("enable_logs", false, enable_logs,
       "Should we log Redex's optimization decisions?");
}

void IRTypeCheckerConfig::bind_config() {
  bind("run_after_each_pass", {}, run_after_each_pass);
  bind("verify_moves", {}, verify_moves);
  bind("validate_invoke_super", {}, validate_invoke_super);
  bind("run_after_passes", {}, run_after_passes);
  bind("check_no_overwrite_this", {}, check_no_overwrite_this);
  bind("annotated_cfg_on_error", annotated_cfg_on_error,
       annotated_cfg_on_error);
  bind("check_classes", {}, check_classes);
  bind("run_on_input", {}, run_on_input);
  bind("run_after_all_passes", {}, run_after_all_passes);
}

void HasherConfig::bind_config() {
  bind("run_after_each_pass", {}, run_after_each_pass);
}

void AssessorConfig::bind_config() {
  bind("run_after_each_pass", run_after_each_pass, run_after_each_pass);
  bind("run_initially", run_initially, run_initially);
  bind("run_finally", run_finally, run_finally);
  bind("run_sb_consistency", run_sb_consistency, run_sb_consistency);
}

void CheckUniqueDeobfuscatedNamesConfig::bind_config() {
  bind("run_after_each_pass", run_after_each_pass, run_after_each_pass);
  bind("run_initially", run_initially, run_initially);
  bind("run_finally", run_finally, run_finally);
}

void MethodProfileOrderingConfig::bind_config() {
  bind("method_sorting_allowlisted_substrings",
       method_sorting_allowlisted_substrings,
       method_sorting_allowlisted_substrings);
  bind("min_appear_percent", min_appear_percent, min_appear_percent);
  bind("second_min_appear_percent", second_min_appear_percent,
       second_min_appear_percent);
  bind("skip_similarity_reordering", skip_similarity_reordering,
       skip_similarity_reordering);
}

void MethodSimilarityOrderingConfig::bind_config() {
  bind("use_class_level_perf_sensitivity", use_class_level_perf_sensitivity,
       use_class_level_perf_sensitivity);
  bind("use_compression_conscious_order", use_compression_conscious_order,
       use_compression_conscious_order);
  bind("disable", disable, disable);
  bind("store_name_to_disable", store_name_to_disable, store_name_to_disable);
}

void ProguardConfig::bind_config() {
  bind("blocklist", blocklist, blocklist);
  bind("disable_default_blocklist", disable_default_blocklist,
       disable_default_blocklist);
  bind("fail_on_unknown_commands", fail_on_unknown_commands,
       fail_on_unknown_commands);
  bind("frozen_basedirectory", frozen_basedirectory, frozen_basedirectory,
       "When set, ignore -basedirectory directives in the proguard "
       "configuration file and use the give value instead.");
}

void PassManagerConfig::bind_config() {
  bind("pass_aliases", pass_aliases, pass_aliases);
  bind("jemalloc_full_stats", jemalloc_full_stats, jemalloc_full_stats);
  bind("violations_tracking", violations_tracking, violations_tracking);
  bind("check_pass_order_properties", check_pass_order_properties,
       check_pass_order_properties);
  bind("check_properties_deep", check_properties_deep, check_properties_deep);
  bind("dump_mrefs", dump_mrefs, dump_mrefs);
}

void ResourceConfig::bind_config() {
  bind("customized_r_classes", {}, customized_r_classes);
  bind("canonical_entry_types", {}, canonical_entry_types);
  bind("sort_key_strings", false, sort_key_strings);
}

void DexOutputConfig::bind_config() {
  bind("write_class_sizes", write_class_sizes, write_class_sizes);
}

void JarLoaderConfig::bind_config() {
  bind("legacy_mode", legacy_mode, legacy_mode);
  bind("allowed_prefixes", allowed_prefixes, allowed_prefixes);
}

void GlobalConfig::bind_config() {
  bool bool_param;
  std::string string_param;
  std::unordered_map<std::string, std::string> string_map_param;
  std::vector<std::string> string_vector_param;
  uint32_t uint32_param;
  Json::Value json_param;
  // Sorted alphabetically
  bind("agg_method_stats_files", {}, string_vector_param);
  bind("baseline_profile_agg_method_stats_files", {}, string_vector_param);
  bind("android_sdk_api_15_file", "", string_param);
  bind("android_sdk_api_16_file", "", string_param);
  bind("android_sdk_api_17_file", "", string_param);
  bind("android_sdk_api_18_file", "", string_param);
  bind("android_sdk_api_19_file", "", string_param);
  bind("android_sdk_api_21_file", "", string_param);
  bind("android_sdk_api_23_file", "", string_param);
  bind("android_sdk_api_25_file", "", string_param);
  bind("android_sdk_api_26_file", "", string_param);
  bind("android_sdk_api_27_file", "", string_param);
  bind("android_sdk_api_28_file", "", string_param);
  bind("android_sdk_api_29_file", "", string_param);
  bind("bytecode_sort_mode", {}, string_vector_param);
  bind("class_frequencies", "", string_param);
  bind("coldstart_classes", "", string_param);
  bind("coldstart_methods_file", "", string_param);
  bind("compute_xml_reachability", false, bool_param);
  bind("deep_data_enabled_interactions", {}, string_vector_param);
  bind("unused_keep_rule_abort", false, bool_param);
  bind("debug_info_kind", "", string_param);
  bind("default_class_frequencies", "", string_param);
  bind("default_coldstart_classes", "", string_param);
  bind("emit_class_method_info_map", false, bool_param);
  bind("iodi_layer_mode", "full", string_param,
       "IODI layer mode. One of \"full\", \"skip-layer-0-at-api-26\" or "
       "\"always-skip-layer-0\"");
  bind("force_single_dex", false, bool_param);
  bind("emit_incoming_hashes", false, bool_param);
  bind("emit_outgoing_hashes", false, bool_param);
  bind("enable_bitset_constant_propagation", false, bool_param,
       "When true, enable bitset constant propagation");
  bind("ignore_no_keep_rules", {}, bool_param);
  bind("instruction_size_bitwidth_limit", 0u, uint32_param);
  bind("json_serde_supercls", {}, string_vector_param);
  bind("keep_all_annotation_classes", true, bool_param);
  bind("record_accessed_rules", true, bool_param);
  bind("keep_methods", {}, string_vector_param);
  bind("keep_packages", {}, string_vector_param);
  bind("lower_with_cfg", {}, bool_param);
  bind("no_optimizations_annotations", {}, string_vector_param);
  bind("no_optimizations_blocklist", {}, string_vector_param);
  bind("preserve_input_dexes", {}, bool_param);
  bind("proguard_map", "", string_param);
  bind("prune_unexported_components", {}, string_vector_param);
  bind("pure_methods", {}, string_vector_param);
  bind("finalish_field_names", {}, string_vector_param);
  bind("record_keep_reasons", {}, bool_param);
  bind("dump_keep_reasons", {}, bool_param);
  bind("string_sort_mode", "", string_param);
  bind("write_cfg_each_pass", false, bool_param);
  bind("dump_cfg_classes", "", string_param);
  bind("slow_invariants_debug", false, bool_param);
  // Enabled for ease of testing, apps expected to opt-out
  bind("enable_bleeding_edge_app_bundle_support", true, bool_param);
  bind("no_devirtualize_annos", {}, string_vector_param);
  bind("create_init_class_insns", true, bool_param);
  bind("finalize_resource_table", false, bool_param);
  bind("check_required_resources", {}, string_vector_param);
  bind("update_method_profiles_stats", false, bool_param);
  bind("recognize_betamap_coldstart_pct_marker", false, bool_param);
  bind("baseline_profile", {}, json_param);
  bind("baseline_profile_config", "", string_param);
  bind("preprocessed_baseline_profile_directory", "", string_param);
  bind("evaluate_package_name", true, bool_param,
       "When true, AndroidManifest.xml will be consulted for the application "
       "package name, and applied during constant propagation.");
  bind("enforce_class_order", false, bool_param,
       "When true, check class order is obeyed to fulfill dex37 verifier "
       "requirements.");

  for (const auto& entry : m_registry) {
    m_global_configs.emplace(entry.name,
                             entry.bind_operation(this, entry.name));
  }
}

GlobalConfigRegistryEntry::GlobalConfigRegistryEntry(
    const std::string& name, BindOperationFn bind_operation)
    : name(name), bind_operation(std::move(bind_operation)) {}

GlobalConfigRegistry& GlobalConfig::default_registry() {
  static GlobalConfigRegistry registry{
      register_as<InlinerConfig>("inliner"),
      register_as<IRTypeCheckerConfig>("ir_type_checker"),
      register_as<HasherConfig>("hasher"),
      register_as<AssessorConfig>("assessor"),
      register_as<CheckUniqueDeobfuscatedNamesConfig>(
          "check_unique_deobfuscated_names"),
      register_as<OptDecisionsConfig>("opt_decisions"),
      register_as<MethodProfileOrderingConfig>("method_profile_order"),
      register_as<MethodSimilarityOrderingConfig>("method_similarity_order"),
      register_as<ProguardConfig>("proguard"),
      register_as<PassManagerConfig>("pass_manager"),
      register_as<ResourceConfig>("resources"),
      register_as<DexOutputConfig>("dex_output"),
      register_as<JarLoaderConfig>("jar_loader"),
  };
  return registry;
}
