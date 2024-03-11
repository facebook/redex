/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TransformConstClassBranches.h"

#include <algorithm>
#include <mutex>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "DexClass.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "SourceBlocks.h"
#include "StringTreeSet.h"
#include "SwitchEquivFinder.h"
#include "SwitchEquivPrerequisites.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_METHODS_TRANSFORMED = "num_methods_transformed";
constexpr const char* METRIC_CONST_CLASS_INSTRUCTIONS_REMOVED =
    "num_const_class_instructions_removed";
constexpr const char* METRIC_TOTAL_STRING_SIZE = "total_string_size";

struct PassState {
  DexMethodRef* lookup_method;
  bool consider_external_classes;
  size_t min_cases;
  size_t max_cases;
  std::mutex& transforms_mutex;
};

// Denotes a branch within a method that can be successfully
// represented/transformed. Used to gather up possibilities and execute them in
// order of biggest gain. This pass may be configured to run late in the pass
// list, and thus should be mindful for number of fields to create.
struct PendingTransform {
  DexClass* cls;
  DexMethod* method;
  cfg::Block* last_prologue_block;
  IRInstruction* insn;
  size_t insn_idx;
  reg_t determining_reg;
  std::unique_ptr<IRCode> code_copy;
  std::unique_ptr<cfg::ScopedCFG> scoped_cfg;
  std::unique_ptr<SwitchEquivFinder> switch_equiv;
};

struct Stats {
  size_t methods_transformed{0};
  size_t const_class_instructions_removed{0};
  size_t string_tree_size{0};
  Stats& operator+=(const Stats& that) {
    methods_transformed += that.methods_transformed;
    const_class_instructions_removed += that.const_class_instructions_removed;
    string_tree_size += that.string_tree_size;
    return *this;
  }
};

bool should_consider_method(DexMethod* method) {
  auto proto = method->get_proto();
  auto args = proto->get_args();
  for (size_t i = 0; i < args->size(); i++) {
    if (args->at(i) == type::java_lang_Class()) {
      return method->get_code() != nullptr;
    }
  }
  return false;
}

// Like ControlFlowGraph::find_insn, return an iterator to the instruction but
// also spit out the index of the instruction (used for compares if needed).
cfg::InstructionIterator find_insn_and_idx(cfg::ControlFlowGraph& cfg,
                                           IRInstruction* insn,
                                           size_t* insn_idx) {
  size_t idx = 0;
  auto iterable = InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn == insn) {
      *insn_idx = idx;
      return it;
    }
    idx++;
  }
  not_reached();
}

size_t num_const_class_opcodes(const cfg::ControlFlowGraph* cfg) {
  size_t result{0};
  for (auto& mie : InstructionIterable(*cfg)) {
    if (mie.insn->opcode() == OPCODE_CONST_CLASS) {
      result++;
    }
  }
  return result;
}

namespace cp = constant_propagation;

void gather_possible_transformations(
    const PassState& pass_state,
    DexClass* cls,
    DexMethod* method,
    std::vector<PendingTransform>* pending_transforms) {
  // First step is to operate on a simplified copy of the code. If the transform
  // is applicable, this copy will take effect.
  auto code_copy = std::make_unique<IRCode>(*method->get_code());
  SwitchEquivEditor::simplify_moves(code_copy.get());
  auto scoped_cfg = std::make_unique<cfg::ScopedCFG>(code_copy.get());
  auto& cfg = **scoped_cfg;
  // Many checks to see if this method conforms to the types of patterns that
  // are able to be easily represented without losing information. At the time
  // of writing this just looks for 1 branching point in the beginning of the
  // method, but this should get addressed to be applicable anywhere in the
  // method.
  std::vector<cfg::Block*> prologue_blocks;
  if (!gather_linear_prologue_blocks(&cfg, &prologue_blocks)) {
    return;
  }
  auto last_prologue_block = prologue_blocks.back();
  auto last_prologue_insn = last_prologue_block->get_last_insn();
  if (last_prologue_insn->insn->opcode() == OPCODE_SWITCH) {
    // Not the expected form
    return;
  }

  TRACE(CCB, 3, "Checking for const-class branching in %s", SHOW(method));
  auto fixpoint = std::make_shared<cp::intraprocedural::FixpointIterator>(
      cfg, SwitchEquivFinder::Analyzer());
  fixpoint->run(ConstantEnvironment());
  reg_t determining_reg;
  if (!find_determining_reg(*fixpoint, prologue_blocks.back(),
                            &determining_reg)) {
    TRACE(CCB, 2, "Cannot find determining_reg; bailing.");
    return;
  }
  TRACE(CCB, 2, "determining_reg is %d", determining_reg);

  size_t insn_idx{0};
  auto finder = std::make_unique<SwitchEquivFinder>(
      &cfg, find_insn_and_idx(cfg, last_prologue_insn->insn, &insn_idx),
      determining_reg, SwitchEquivFinder::NO_LEAF_DUPLICATION, fixpoint,
      SwitchEquivFinder::EXECUTION_ORDER);
  if (!finder->success() ||
      !finder->are_keys_uniform(SwitchEquivFinder::KeyKind::CLASS)) {
    TRACE(CCB, 2, "SwitchEquivFinder failed!");
    return;
  }
  TRACE(CCB, 2, "SwitchEquivFinder succeeded for branch at: %s",
        SHOW(last_prologue_insn->insn));
  if (!finder->extra_loads().empty()) {
    TRACE(CCB, 2,
          "Not supporting extra const-class loads during switch; bailing.");
    return;
  }

  const auto& key_to_case = finder->key_to_case();
  size_t relevant_case_count{0};
  for (auto&& [key, block] : key_to_case) {
    if (!SwitchEquivFinder::is_default_case(key)) {
      auto dtype = boost::get<const DexType*>(key);
      auto case_class = type_class(dtype);
      if (pass_state.consider_external_classes ||
          (case_class != nullptr && !case_class->is_external())) {
        relevant_case_count++;
      }
    }
  }
  if (finder->default_case() == boost::none) {
    TRACE(CCB, 2, "Default block not found; bailing.");
    return;
  }
  if (relevant_case_count > pass_state.max_cases ||
      relevant_case_count < pass_state.min_cases) {
    TRACE(CCB, 2, "Not operating on method due to size.");
    return;
  }
  // Method should conform to expectations!
  std::lock_guard<std::mutex> lock(pass_state.transforms_mutex);
  PendingTransform t{cls,
                     method,
                     last_prologue_block,
                     last_prologue_insn->insn,
                     insn_idx,
                     determining_reg,
                     std::move(code_copy),
                     std::move(scoped_cfg),
                     std::move(finder)};
  pending_transforms->emplace_back(std::move(t));
}

Stats apply_transform(const PassState& pass_state,
                      PendingTransform& transform) {
  Stats result;
  auto method = transform.method;
  auto& cfg = **transform.scoped_cfg;
  TRACE(CCB, 3, "Transforming const-class branching in %s %s", SHOW(method),
        SHOW(cfg));
  auto finder = transform.switch_equiv.get();

  // Determine stable order of the types that are being switched on.
  std::set<const DexType*, dextypes_comparator> ordered_types;
  const auto& key_to_case = finder->key_to_case();
  cfg::Block* default_case{nullptr};
  for (auto&& [key, block] : key_to_case) {
    if (!SwitchEquivFinder::is_default_case(key)) {
      auto dtype = boost::get<const DexType*>(key);
      ordered_types.emplace(dtype);
    } else {
      TRACE(CCB, 3, "DEFAULT -> B%zu\n%s", block->id(), SHOW(block));
      default_case = block;
    }
  }
  // Create ordinals for each type being switched on, reserving zero to denote
  // an explicit default case.
  auto before_const_class_count = num_const_class_opcodes(&cfg);
  std::map<std::string, int16_t> string_tree_items;
  std::vector<std::pair<int32_t, cfg::Block*>> new_edges;
  constexpr int16_t STRING_TREE_NO_ENTRY = 0;
  int16_t counter = STRING_TREE_NO_ENTRY + 1;
  for (const auto& type : ordered_types) {
    auto string_name = java_names::internal_to_external(type->str_copy());
    int16_t ordinal = counter++;
    string_tree_items.emplace(string_name, ordinal);
    auto block = key_to_case.at(type);
    new_edges.emplace_back(ordinal, block);
    TRACE(CCB, 3, "%s (%s) -> B%zu\n%s", SHOW(type), string_name.c_str(),
          block->id(), SHOW(block));
  }

  // Install a new static string field for the encoded types and their ordinals.
  auto encoded_str =
      StringTreeMap<int16_t>::encode_string_tree_map(string_tree_items);
  result.string_tree_size = encoded_str.size();
  auto encoded_dex_str = DexString::make_string(encoded_str);

  // Fiddle with the prologue block and install an actual switch
  TRACE(CCB, 2, "Removing last prologue instruction: %s", SHOW(transform.insn));

  std::vector<IRInstruction*> replacements;
  auto encoded_str_reg = cfg.allocate_temp();
  auto const_string_insn =
      (new IRInstruction(OPCODE_CONST_STRING))->set_string(encoded_dex_str);
  replacements.push_back(const_string_insn);
  auto move_string_insn = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                              ->set_dest(encoded_str_reg);
  replacements.push_back(move_string_insn);

  auto default_value_reg = cfg.allocate_temp();
  auto default_value_const = new IRInstruction(OPCODE_CONST);
  default_value_const->set_literal(STRING_TREE_NO_ENTRY);
  default_value_const->set_dest(default_value_reg);
  replacements.push_back(default_value_const);

  auto invoke_string_tree = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_string_tree->set_method(pass_state.lookup_method);
  invoke_string_tree->set_srcs_size(3);
  invoke_string_tree->set_src(0, transform.determining_reg);
  invoke_string_tree->set_src(1, encoded_str_reg);
  invoke_string_tree->set_src(2, default_value_reg);
  replacements.push_back(invoke_string_tree);

  // Just reuse a reg we don't need anymore
  auto switch_result_reg = default_value_reg;
  auto move_lookup_result = new IRInstruction(OPCODE_MOVE_RESULT);
  move_lookup_result->set_dest(switch_result_reg);
  replacements.push_back(move_lookup_result);

  auto new_switch = new IRInstruction(OPCODE_SWITCH);
  new_switch->set_src(0, switch_result_reg);
  // Note: it seems instruction "new_switch" gets appended via create_branch; no
  // need to push to replacements

  cfg.replace_insns(cfg.find_insn(transform.insn), replacements);
  // We are explicitly covering the default block via the default return value
  // from the string tree. Not needed here.
  cfg.create_branch(transform.last_prologue_block, new_switch, nullptr,
                    new_edges);

  // Reset successor of last prologue block to implement the default case.
  for (auto& edge : transform.last_prologue_block->succs()) {
    if (edge->type() == cfg::EDGE_GOTO) {
      cfg.set_edge_target(edge, default_case);
    }
  }

  // Last step is to prune leaf blocks which are now unreachable. Do this before
  // computing metrics (so we know if this pass is doing anything useful) but
  // be sure to not dereference any Block ptrs from here on out!
  cfg.remove_unreachable_blocks();
  TRACE(CCB, 3, "POST EDIT %s", SHOW(cfg));
  result.methods_transformed = 1;
  // Metric is not entirely accurate as we don't do dce on the prologue block
  // (eehhh close enough).
  result.const_class_instructions_removed =
      before_const_class_count - num_const_class_opcodes(&cfg);
  always_assert(result.const_class_instructions_removed >= 0);

  // Make the copy take effect.
  transform.method->set_code(std::move(transform.code_copy));
  return result;
}

class TransformConstClassBranchesInterDexPlugin
    : public interdex::InterDexPassPlugin {
 public:
  explicit TransformConstClassBranchesInterDexPlugin() {}

  ReserveRefsInfo reserve_refs() override {
    return ReserveRefsInfo(/* frefs */ 0,
                           /* trefs */ 0,
                           /* mrefs */ 2);
  }
};
} // namespace

void TransformConstClassBranchesPass::bind_config() {
  bind("consider_external_classes", false, m_consider_external_classes);
  // Probably not worthwhile for tiny methods.
  bind("min_cases", 5, m_min_cases);
  // Arbitrary default values to avoid creating unbounded amounts of encoded
  // string data.
  bind("max_cases", 2000, m_max_cases);
  bind("string_tree_lookup_method", "", m_string_tree_lookup_method);
  trait(Traits::Pass::unique, true);

  after_configuration([] {
    interdex::InterDexRegistry* registry =
        static_cast<interdex::InterDexRegistry*>(
            PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
    std::function<interdex::InterDexPassPlugin*()> fn =
        []() -> interdex::InterDexPassPlugin* {
      return new TransformConstClassBranchesInterDexPlugin();
    };
    registry->register_plugin("TRANSFORM_CONST_CLASS_BRANCHES_PLUGIN",
                              std::move(fn));
  });
}

void TransformConstClassBranchesPass::run_pass(DexStoresVector& stores,
                                               ConfigFiles& /* unused */,
                                               PassManager& mgr) {
  auto scope = build_class_scope(stores);
  if (m_string_tree_lookup_method.empty()) {
    TRACE(CCB, 1, "Pass not configured; returning.");
    return;
  }
  auto string_tree_lookup_method =
      DexMethod::get_method(m_string_tree_lookup_method);
  if (string_tree_lookup_method == nullptr) {
    TRACE(CCB, 1, "Lookup method not found; returning.");
    return;
  }

  std::vector<PendingTransform> transforms;
  std::mutex transforms_mutex;
  PassState pass_state{string_tree_lookup_method, m_consider_external_classes,
                       m_min_cases, m_max_cases, transforms_mutex};
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (should_consider_method(method)) {
      gather_possible_transformations(
          pass_state, type_class(method->get_class()), method, &transforms);
    }
  });

  Stats stats;
  for (auto& transform : transforms) {
    stats += apply_transform(pass_state, transform);
  }
  mgr.incr_metric(METRIC_METHODS_TRANSFORMED, stats.methods_transformed);
  mgr.incr_metric(METRIC_CONST_CLASS_INSTRUCTIONS_REMOVED,
                  stats.const_class_instructions_removed);
  mgr.incr_metric(METRIC_TOTAL_STRING_SIZE, stats.string_tree_size);
  TRACE(CCB, 1,
        "[transform const-class branches] Altered %zu method(s) to remove %zu "
        "const-class instructions; %zu bytes of character data created.",
        stats.methods_transformed, stats.const_class_instructions_removed,
        stats.string_tree_size);
}

static TransformConstClassBranchesPass s_pass;
