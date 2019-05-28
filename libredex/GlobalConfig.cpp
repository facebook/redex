/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalConfig.h"

void InlinerConfig::bind_config() {
  bind("use_cfg_inliner", use_cfg_inliner, use_cfg_inliner);
  bind("enforce_method_size_limit", enforce_method_size_limit,
       enforce_method_size_limit);
  bind("throws", throws_inline, throws_inline);
  bind("multiple_callers", multiple_callers, multiple_callers);
  bind("inline_small_non_deletables", inline_small_non_deletables,
       inline_small_non_deletables);
  bind("no_inline_annos", {}, m_no_inline_annos);
  bind("force_inline_annos", {}, m_force_inline_annos);
  bind("black_list", {}, m_black_list);
  bind("black_list", {}, m_black_list);
  bind("caller_black_list", {}, m_caller_black_list);
}

void OptDecisionsConfig::bind_config() {
  bind("enable_logs", false, enable_logs,
       "Should we log Redex's optimization decisions?");
  bind("output_file_name", "", output_file_name,
       "Filename that optimization decisions will be logged too.");
}

void IRTypeCheckerConfig::bind_config() {
  bind("run_after_each_pass", {}, run_after_each_pass);
  bind("verify_moves", {}, verify_moves);
  bind("run_after_passes", {}, run_after_passes);
}

void GlobalConfig::bind_config() {
  OptDecisionsConfig opt_decisions_param;
  IRTypeCheckerConfig ir_type_checker_param;
  InlinerConfig inliner_param;
  bool bool_param;
  std::string string_param;
  std::vector<std::string> string_vector_param;
  bind("inliner", InlinerConfig(), inliner_param);
  bind("opt_decisions", OptDecisionsConfig(), opt_decisions_param);
  bind("ir_type_checker", IRTypeCheckerConfig(), ir_type_checker_param);
  bind("lower_with_cfg", {}, bool_param);
  bind("emit_locator_strings", {}, bool_param);
  bind("emit_name_based_locator_strings", {}, bool_param);
  bind("record_keep_reasons", {}, bool_param);
  bind("debug_info_kind", "", string_param);
  bind("proguard_map_output", "", string_param);
  bind("bytecode_offset_map", "", string_param);
  bind("class_method_info_map", "", string_param);
  bind("bytecode_sort_mode", "", string_param);
  bind("keep_packages", {}, string_vector_param);
  bind("keep_annotations", {}, string_vector_param);
  bind("keep_class_members", {}, string_vector_param);
  bind("json_serde_supercls", {}, string_vector_param);
  bind("no_optimizations_annotations", {}, string_vector_param);
  bind("method_sorting_whitelisted_substrings", {}, string_vector_param);
  bind("prune_unexported_components", {}, string_vector_param);
}
