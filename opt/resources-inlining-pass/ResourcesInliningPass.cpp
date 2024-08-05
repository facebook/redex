/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <json/value.h>
#include <set>

#include "CFGMutation.h"
#include "ConfigFiles.h"
#include "ConstantPropagationAnalysis.h"
#include "PassManager.h"
#include "ResourcesInliningPass.h"
#include "Trace.h"
#include "Walkers.h"

std::unordered_map<uint32_t, resources::InlinableValue>
ResourcesInliningPass::filter_inlinable_resources(
    ResourceTableFile* res_table,
    const std::unordered_map<uint32_t, resources::InlinableValue>&
        inlinable_resources,
    const std::unordered_set<std::string>& resource_type_names,
    const std::unordered_set<std::string>& resource_entry_names) {
  auto type_ids = res_table->get_types_by_name(resource_type_names);

  std::vector<std::string> type_names;
  res_table->get_type_names(&type_names);

  const auto& id_to_name = res_table->id_to_name;
  std::unordered_map<uint32_t, resources::InlinableValue>
      refined_inlinable_resources;

  for (auto& pair : inlinable_resources) {
    auto& id = pair.first;
    auto& value = pair.second;

    auto masked_type = id & 0x00FF0000;
    const auto& id_name = id_to_name.at(id);

    std::string type_name =
        type_names[(masked_type >> TYPE_INDEX_BIT_SHIFT) - 1];
    std::string entry_name_formatted = type_name + "/" + id_name;

    if (type_ids.find(masked_type) != type_ids.end() ||
        resource_entry_names.find(entry_name_formatted) !=
            resource_entry_names.end()) {
      refined_inlinable_resources.insert({id, value});
    }
  }

  return refined_inlinable_resources;
}

void ResourcesInliningPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  std::string zip_dir;
  conf.get_json_config().get("apk_dir", "", zip_dir);
  always_assert(!zip_dir.empty());
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();
  auto inlinable = res_table->get_inlinable_resource_values();
  std::unordered_map<uint32_t, resources::InlinableValue> inlinable_resources =
      filter_inlinable_resources(res_table.get(),
                                 inlinable,
                                 m_resource_type_names,
                                 m_resource_entry_names);

  const Scope scope = build_class_scope(stores);

  MethodTransformsMap possible_transformations =
      ResourcesInliningPass::find_transformations(scope, inlinable_resources);

  for (auto& pair : possible_transformations) {
    auto method = pair.first;
    auto& transforms = pair.second;
    ResourcesInliningPass::inline_resource_values_dex(method, transforms, mgr);
  }
}

/*
 This method generates a map of the valid APIs that can be inlined to the
 range of valid types
 that can be inlined. The DexMethodRef* represents the method that is being
 called and the first component of the tuple
 represents the lower bound of the type that can be inlined and the second
 component represents the upper bound of the type that can be inlined. Per the
 following Android source links, these methods are performing no further logic
 beyond retrieving the raw data from the resource table and thus should be
 easily representable with dex instructions.
 https://cs.android.com/android/platform/superproject/+/android-14.0.0_r1:frameworks/base/core/java/android/content/res/Resources.java;l=1180
 https://cs.android.com/android/platform/superproject/+/android-14.0.0_r1:frameworks/base/core/java/android/content/res/Resources.java;l=1073
 https://cs.android.com/android/platform/superproject/+/android-14.0.0_r1:frameworks/base/core/java/android/content/res/Resources.java;l=1206
*/
std::unordered_map<DexMethodRef*, std::tuple<uint8_t, uint8_t>>
generate_valid_apis() {
  std::unordered_map<DexMethodRef*, std::tuple<uint8_t, uint8_t>> usable_apis;

  DexMethodRef* bool_method =
      DexMethod::get_method("Landroid/content/res/Resources;.getBoolean:(I)Z");
  std::tuple<uint8_t, uint8_t> bool_range =
      std::make_tuple(android::Res_value::TYPE_INT_BOOLEAN,
                      android::Res_value::TYPE_INT_BOOLEAN);
  usable_apis.insert({bool_method, bool_range});

  DexMethodRef* color_method =
      DexMethod::get_method("Landroid/content/res/Resources;.getColor:(I)I");
  std::tuple<uint8_t, uint8_t> color_range =
      std::make_tuple(android::Res_value::TYPE_FIRST_COLOR_INT,
                      android::Res_value::TYPE_LAST_COLOR_INT);
  usable_apis.insert({color_method, color_range});

  DexMethodRef* int_method =
      DexMethod::get_method("Landroid/content/res/Resources;.getInteger:(I)I");
  std::tuple<uint8_t, uint8_t> int_range = std::make_tuple(
      android::Res_value::TYPE_INT_DEC, android::Res_value::TYPE_INT_HEX);
  usable_apis.insert({int_method, int_range});

  DexMethodRef* string_method = DexMethod::get_method(
      "Landroid/content/res/Resources;.getString:(I)Ljava/lang/String;");
  std::tuple<uint8_t, uint8_t> string_range = std::make_tuple(
      android::Res_value::TYPE_STRING, android::Res_value::TYPE_STRING);
  usable_apis.insert({string_method, string_range});

  return usable_apis;
}

bool exists_possible_transformation(
    const cfg::ControlFlowGraph& cfg,
    const std::unordered_map<DexMethodRef*, std::tuple<uint8_t, uint8_t>>&
        dex_method_refs) {
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
          (dex_method_refs.find(insn->get_method()) != dex_method_refs.end())) {
        return true;
      }
    }
  }
  return false;
}

MethodTransformsMap ResourcesInliningPass::find_transformations(
    const Scope& scope,
    const std::unordered_map<uint32_t, resources::InlinableValue>&
        inlinable_resources) {

  std::unordered_map<DexMethodRef*, std::tuple<uint8_t, uint8_t>>
      dex_method_refs = generate_valid_apis();

  MethodTransformsMap possible_transformations;

  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (method->rstate.no_optimizations()) {
      return;
    }

    namespace cp = constant_propagation;
    using CombinedAnalyzer =
        InstructionAnalyzerCombiner<cp::StaticFinalFieldAnalyzer,
                                    cp::HeapEscapeAnalyzer,
                                    cp::PrimitiveAnalyzer>;

    // Retrieving cfg
    auto get_code = method->get_code();
    if (get_code == nullptr) {
      return;
    }
    auto& cfg = get_code->cfg();

    if (!exists_possible_transformation(cfg, dex_method_refs)) {
      return;
    }

    TRACE(RIP, 1, "Found possible transformations for %s", SHOW(method));
    cp::intraprocedural::FixpointIterator intra_cp(
        /* cp_state */ nullptr, cfg,
        CombinedAnalyzer(nullptr, nullptr, nullptr));
    // Runing the combined analyzer initially
    intra_cp.run(ConstantEnvironment());

    std::vector<InlinableOptimization> transforms;
    // Looping through each block and replaying
    for (auto* block : cfg.blocks()) {
      auto env = intra_cp.get_entry_state_at(block);
      auto last_insn = block->get_last_insn();
      // Going through each instruction in the block and checking for invoke
      // virtual, if it is inlinable and if it is a valid API call
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
            (dex_method_refs.find(insn->get_method()) !=
             dex_method_refs.end())) {
          auto field_domain = env.get<SignedConstantDomain>(insn->src(1));
          auto const_value = field_domain.get_constant();
          if (const_value != boost::none &&
              inlinable_resources.find(const_value.value()) !=
                  inlinable_resources.end()) {
            // Adding to list of possible optimizations if it is
            auto insertable = InlinableOptimization();
            insertable.insn = insn;
            insertable.inlinable_value =
                inlinable_resources.at(const_value.value());
            transforms.push_back(insertable);
          }
        }
        intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
      }
    }
    // For each method, adding all possible transformations to the map
    if (!transforms.empty()) {
      possible_transformations.emplace(method, std::move(transforms));
    }
  });
  return possible_transformations;
}

void ResourcesInliningPass::inline_resource_values_dex(
    DexMethod* method,
    const std::vector<InlinableOptimization>& insn_inlinable,
    PassManager& mgr) {
  auto& cfg = method->get_code()->cfg();
  cfg::CFGMutation mutator(cfg);

  std::unordered_map<DexMethodRef*, std::tuple<uint8_t, uint8_t>> usable_apis =
      generate_valid_apis();

  IRInstruction* new_insn;
  for (const auto& elem : insn_inlinable) {
    auto insn = elem.insn;
    auto inlinable_value = elem.inlinable_value;
    cfg::InstructionIterator it_invoke = cfg.find_insn(insn);
    DexMethodRef* method_ref = insn->get_method();
    auto method_bounds = usable_apis.at(method_ref);
    auto method_lower_bound = get<0>(method_bounds);
    auto method_upper_bound = get<1>(method_bounds);

    if (method_lower_bound > inlinable_value.type ||
        method_upper_bound < inlinable_value.type) {
      continue;
    }

    auto move_insn_it = cfg.move_result_of(it_invoke);
    if (move_insn_it.is_end()) {
      mgr.incr_metric("removed_unused_invokes", 1);
      mutator.remove(it_invoke);
      continue;
    }
    auto move_insn = move_insn_it->insn;

    if (move_insn->opcode() == OPCODE_MOVE_RESULT) {
      new_insn = new IRInstruction(OPCODE_CONST);
      if (inlinable_value.type == android::Res_value::TYPE_INT_BOOLEAN) {
        new_insn->set_literal(inlinable_value.bool_value);
        mgr.incr_metric("inlined_booleans", 1);
      } else {
        new_insn->set_literal((int32_t)inlinable_value.uint_value);
        mgr.incr_metric("inlined_integers", 1);
      }
      always_assert_log(move_insn->has_dest(),
                        "The move instruction has no destination");
      new_insn->set_dest(move_insn->dest());
      mutator.replace(it_invoke, {new_insn});
    }

    else if (move_insn->opcode() == OPCODE_MOVE_RESULT_OBJECT) {
      new_insn = new IRInstruction(OPCODE_CONST_STRING);
      new_insn->set_string(
          DexString::make_string(inlinable_value.string_value));
      IRInstruction* new_insn_pseudo_move =
          new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
      always_assert_log(move_insn->has_dest(),
                        "The move instruction has no destination");
      new_insn_pseudo_move->set_dest(move_insn->dest());
      mutator.replace(it_invoke, {new_insn, new_insn_pseudo_move});
      mgr.incr_metric("inlined_strings", 1);
    }

    mutator.remove(move_insn_it);
    mgr.incr_metric("inlined_total", 1);
  }
  mutator.flush();
}

static ResourcesInliningPass s_pass;
