/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ModelMethodMerger.h"

#include "AnnoUtils.h"
#include "ConstantLifting.h"
#include "Creators.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Inliner.h"
#include "MethodDedup.h"
#include "MethodReference.h"
#include "Mutators.h"
#include "Resolver.h"
#include "Show.h"
#include "SwitchDispatch.h"
#include "TypeReference.h"
#include "Walkers.h"

using namespace class_merging;

namespace {

using MethodTypeTags = std::unordered_map<const DexMethod*, uint32_t>;

const size_t CONST_LIFT_STUB_THRESHOLD = 2;

void update_call_refs(
    const method_reference::CallSites& call_sites,
    const MethodTypeTags& type_tags,
    const std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee,
    bool with_type_tag = false) {
  for (const auto& callsite : call_sites) {
    auto callee = callsite.callee;
    always_assert(callee != nullptr && type_tags.count(callee) > 0);
    auto new_callee_method = old_to_new_callee.at(callee);
    auto type_tag_arg = with_type_tag
                            ? boost::optional<uint32_t>(type_tags.at(callee))
                            : boost::none;
    method_reference::NewCallee new_callee(new_callee_method, type_tag_arg);
    patch_callsite(callsite, new_callee);
  }
}

/**
 * Staticize the method and replace its first parameter with a new type.
 */
void staticize_with_new_arg_head(DexMethod* meth, DexType* new_head) {
  mutators::make_static(meth, mutators::KeepThis::Yes);
  DexMethodSpec spec;
  auto args = meth->get_proto()->get_args();
  always_assert(args->size());
  auto new_type_list = args->replace_head(new_head);
  auto new_proto =
      DexProto::make_proto(meth->get_proto()->get_rtype(), new_type_list);
  spec.proto = new_proto;
  if (method::is_init(meth)) {
    // <init> can not be renamed on collision, change it to a plain name.
    spec.name = DexString::make_string("_init_");
  }
  meth->change(spec, true /* rename on collision */);
}

template <typename T>
void fix_visibility_helper(DexMethod* method, T& vmethods_created) {
  // Fix non-static non-ctor private callees
  for (auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    auto opcode = insn->opcode();
    if (!opcode::is_invoke_direct(opcode)) {
      continue;
    }
    auto callee = resolve_method(insn->get_method(), MethodSearch::Direct);
    if (callee == nullptr || !callee->is_concrete() ||
        method::is_any_init(callee) || is_public(callee)) {
      continue;
    }
    always_assert(is_private(callee));
    auto cls = type_class(callee->get_class());
    cls->remove_method(callee);
    callee->set_virtual(true);
    set_public(callee);
    cls->add_method(callee);
    vmethods_created.insert(callee);
  }
  // Fix the rest
  change_visibility(method);
}

boost::optional<size_t> get_ctor_type_tag_param_idx(
    const bool pass_type_tag_param, const DexProto* ctor_proto) {
  boost::optional<size_t> type_tag_param_idx = boost::none;
  if (pass_type_tag_param) {
    return type_tag_param_idx;
  }

  size_t idx = 0;
  for (auto type : *ctor_proto->get_args()) {
    if (type == type::_int()) {
      always_assert_log(!type_tag_param_idx,
                        "More than one potential type tag param found!");
      type_tag_param_idx = boost::optional<size_t>(idx);
    }
    ++idx;
  }
  return type_tag_param_idx;
}

/**
 * Return switch block from the incoming cfg if it only contains one.
 */
cfg::Block* find_single_switch(const cfg::ControlFlowGraph& cfg) {
  cfg::Block* switch_block = nullptr;

  for (const auto& block : cfg.blocks()) {
    for (auto& mei : InstructionIterable(block)) {
      if (opcode::is_switch(mei.insn->opcode())) {
        if (!switch_block) {
          switch_block = block;
        } else {
          // must only contain a single switch
          return nullptr;
        }
      }
    }
  }
  return switch_block;
}

/**
 * If there are common ctor invocations in each switch statements,
 * put them into invocations vector.
 */
static void find_common_ctor_invocations(
    cfg::Block* switch_block,
    cfg::Block* return_block,
    DexMethod*& common_ctor,
    std::vector<IRList::iterator>& invocations) {
  // edges could point to the same target, but we only care unique targets.
  std::unordered_set<cfg::Block*> targets;
  for (auto& s : switch_block->succs()) {
    targets.insert(s->target());
  }
  if (targets.size() <= 1) return;

  for (auto& target : targets) {
    if (return_block != target->goes_to_only_edge()) {
      // not all switch statements goto return block
      invocations.clear();
      return;
    }
    auto last_non_goto_insn = target->get_last_insn();
    if (opcode::is_goto(last_non_goto_insn->insn->opcode())) {
      do {
        assert_log(last_non_goto_insn != target->get_first_insn(),
                   "Should have at least one non-goto opcode!");
        last_non_goto_insn = std::prev(last_non_goto_insn);
      } while (last_non_goto_insn->type != MFLOW_OPCODE);
    }
    if (!opcode::is_invoke_direct(last_non_goto_insn->insn->opcode())) {
      invocations.clear();
      return;
    }

    auto meth = resolve_method(last_non_goto_insn->insn->get_method(),
                               MethodSearch::Direct);
    // Make sure we found the same init method
    if (!meth || !method::is_init(meth) ||
        (common_ctor && common_ctor != meth)) {
      invocations.clear();
      return;
    }
    common_ctor = meth;
    invocations.emplace_back(last_non_goto_insn);
  }
}
} // namespace

namespace class_merging {

void MethodStats::add(const MethodOrderedSet& methods) {
  std::unordered_map<std::string, size_t> method_counts;
  std::unordered_map<std::string, std::vector<std::string>> samples;
  for (auto& m : methods) {
    auto simple_name = m->get_simple_deobfuscated_name();
    if (simple_name.find("get") == 0 || simple_name.find("set") == 0) {
      simple_name = simple_name.substr(0, 3);
    } else if (simple_name.find("$dispatch$") == 0) {
      simple_name = simple_name.substr(0, 10);
    }
    auto name = simple_name + show(m->get_proto());
    method_counts[name]++;
    if (samples.count(name) == 0 || samples[name].size() < 3) {
      std::string sample_str = show_deobfuscated(m) + "\n";
      sample_str += show(m->get_code());
      samples[name].push_back(std::move(sample_str));
    }
  }
  for (auto& it : method_counts) {
    auto name = it.first;
    auto count = it.second;
    MergedMethod mm{name, count, samples[name]};
    merged_methods.push_back(mm);
  }
}

void MethodStats::print(const std::string& model_name,
                        uint32_t num_mergeables) {
  if (!traceEnabled(CLMG, 8)) {
    return;
  }
  TRACE(CLMG,
        8,
        "==== methods stats for %s (%u) ====",
        model_name.c_str(),
        num_mergeables);
  for (auto& mm : merged_methods) {
    TRACE(CLMG, 8, " %4zu %s", mm.count, mm.name.c_str());
    if (mm.count > 1) {
      for (const auto& sample : mm.samples) {
        TRACE(CLMG, 9, "%s", sample.c_str());
      }
    }
  }
}

ModelMethodMerger::ModelMethodMerger(
    const Scope& scope,
    const std::vector<const MergerType*>& mergers,
    const MergerToField& type_tag_fields,
    const TypeTags* type_tags,
    const std::unordered_map<DexMethod*, std::string>& method_debug_map,
    const ModelSpec& model_spec,
    boost::optional<size_t> max_num_dispatch_target)
    : m_scope(scope),
      m_mergers(mergers),
      m_type_tag_fields(type_tag_fields),
      m_type_tags(type_tags),
      m_method_debug_map(method_debug_map),
      m_model_spec(model_spec),
      m_max_num_dispatch_target(max_num_dispatch_target) {
  for (const auto& mtf : type_tag_fields) {
    auto type_tag_field = mtf.second;
    if (model_spec.generate_type_tag()) {
      always_assert(type_tag_field && type_tag_field->is_concrete());
    }
  }
  // Collect ctors, non_ctors.
  for (const MergerType* merger : mergers) {
    std::vector<DexMethod*> ctors;
    std::vector<DexMethod*> non_ctors;
    for (const auto m : merger->dmethods) {
      if (method::is_init(m)) {
        ctors.push_back(m);
      } else if (!method::is_clinit(m)) {
        non_ctors.push_back(m);
      }
    }
    m_merger_ctors[merger] = ctors;
    m_merger_non_ctors[merger] = non_ctors;
    m_merger_non_vmethods[merger] = merger->non_virt_methods;
  }
  fix_visibility();
  m_method_stats = {{}};
}

void ModelMethodMerger::fix_visibility() {
  std::unordered_set<DexMethod*> vmethods_created;
  for (const auto& pair : m_merger_ctors) {
    const std::vector<DexMethod*>& ctors = pair.second;
    for (DexMethod* m : ctors) {
      fix_visibility_helper(m, vmethods_created);
    }
  }
  for (const auto& pair : m_merger_non_ctors) {
    const std::vector<DexMethod*>& non_ctors = pair.second;
    for (auto m : non_ctors) {
      fix_visibility_helper(m, vmethods_created);
    }
  }
  for (const auto& pair : m_merger_non_vmethods) {
    auto non_vmethods = pair.second;
    for (auto m : non_vmethods) {
      fix_visibility_helper(m, vmethods_created);
    }
  }
  for (auto merger : m_mergers) {
    for (auto& vm_lst : merger->vmethods) {
      for (auto m : vm_lst.overrides) {
        fix_visibility_helper(m, vmethods_created);
      }
    }
    for (const auto& im : merger->intfs_methods) {
      for (auto m : im.methods) {
        fix_visibility_helper(m, vmethods_created);
      }
    }
  }
  // Promote privatized non-static non-ctor methods back to be public virtual.
  for (const auto& pair : m_merger_non_ctors) {
    auto non_ctors = pair.second;
    for (const auto m : non_ctors) {
      if (is_private(m) && !is_static(m)) {
        auto cls = type_class(m->get_class());
        cls->remove_method(m);
        m->set_virtual(true);
        set_public(m);
        cls->add_method(m);
        vmethods_created.insert(m);
      }
    }
  }
  // Fix call sites of vmethods_created.
  auto call_sites =
      method_reference::collect_call_refs(m_scope, vmethods_created);
  for (const auto& callsite : call_sites) {
    auto insn = callsite.mie->insn;
    always_assert(opcode::is_invoke_direct(insn->opcode()));
    insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
  }
}

std::vector<IRInstruction*> ModelMethodMerger::make_string_const(
    reg_t dest, const std::string& val) {
  std::vector<IRInstruction*> res;
  IRInstruction* load = new IRInstruction(OPCODE_CONST_STRING);
  load->set_string(DexString::make_string(val));
  IRInstruction* move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  move_result_pseudo->set_dest(dest);
  res.push_back(load);
  res.push_back(move_result_pseudo);
  return res;
}

std::vector<IRInstruction*> ModelMethodMerger::make_check_cast(DexType* type,
                                                               reg_t src_dest) {
  auto check_cast = new IRInstruction(OPCODE_CHECK_CAST);
  check_cast->set_type(type)->set_src(0, src_dest);
  auto move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  move_result_pseudo->set_dest(src_dest);
  return {check_cast, move_result_pseudo};
}

dispatch::DispatchMethod ModelMethodMerger::create_dispatch_method(
    const dispatch::Spec& spec, const std::vector<DexMethod*>& targets) {
  always_assert(targets.size());
  TRACE(CLMG,
        5,
        "creating dispatch %s.%s for targets of size %zu",
        SHOW(spec.owner_type),
        spec.name.c_str(),
        targets.size());

  // Setup switch cases
  // The MethodBlocks are to be initialized by switch_op() based on their
  // corresponding keys in the map.
  auto indices_to_callee = get_dedupped_indices_map(targets);
  m_stats.m_num_vmethods_dedupped += targets.size() - indices_to_callee.size();
  return create_virtual_dispatch(spec, indices_to_callee);
}

std::map<SwitchIndices, DexMethod*> ModelMethodMerger::get_dedupped_indices_map(
    const std::vector<DexMethod*>& targets) {
  always_assert(targets.size());
  std::map<SwitchIndices, DexMethod*> indices_to_callee;

  // Find equivalent methods.
  std::vector<MethodOrderedSet> duplicates =
      method_dedup::group_identical_methods(
          targets, m_model_spec.dedup_fill_in_stack_trace);
  for (const auto& duplicate : duplicates) {
    SwitchIndices switch_indices;
    for (auto& meth : duplicate) {
      switch_indices.emplace(m_type_tags->get_type_tag(meth->get_class()));
    }
    indices_to_callee[switch_indices] = *duplicate.begin();
  }

  TRACE(CLMG, 9, "---- SwitchIndices map ---");
  for (auto& it : indices_to_callee) {
    auto indices = it.first;
    auto callee = it.second;
    TRACE(CLMG, 9, "indices %s callee %s", SHOW(indices), SHOW(callee));
  }
  return indices_to_callee;
}

DexType* ModelMethodMerger::get_merger_type(DexType* mergeable) {
  auto merger_ctor = m_mergeable_to_merger_ctor.at(mergeable);
  return merger_ctor->get_class();
}

/**
 * For a merged constructor, if every switch statement ends up calling the same
 * super constructor, we sink them to one invocation at the return block right
 * after the switch statements:
 *
 * switch (typeTag) {                   switch (typeTag) {
 *   case ATypeTag:                       case ATypeTag:
 *     // do something for A                // do something for A
 *     super(num);                          break;
 *     break;                  ==>        case BTypeTag:
 *   case BTypeTag:                         // do something for B
 *     // do something for B                break;
 *     super(num);                      }
 *     break;                           super(num);
 * }
 */
void ModelMethodMerger::sink_common_ctor_to_return_block(DexMethod* dispatch) {
  auto dispatch_code = dispatch->get_code();
  // TODO (cnli): use editable CFG and update insert logic.
  dispatch_code->build_cfg(/* editable */ false);
  const auto& cfg = dispatch_code->cfg();
  if (cfg.return_blocks().size() != 1) {
    dispatch_code->clear_cfg();
    return;
  }
  auto return_block = cfg.return_blocks()[0];

  auto switch_block = find_single_switch(cfg);
  if (!switch_block) {
    dispatch_code->clear_cfg();
    return;
  }

  std::vector<IRList::iterator> invocations;
  DexMethod* common_ctor = nullptr;
  find_common_ctor_invocations(switch_block, return_block, common_ctor,
                               invocations);
  if (invocations.empty()) {
    dispatch_code->clear_cfg();
    return;
  }
  assert(common_ctor != nullptr);

  // Move args in common ctor to the same registers in all statements.
  // For example:
  //
  // case_block_1:                         case_block_1:
  // MOVE_OBJECT v3, v0                    MOVE_OBJECT v3, v0
  // MOVE v4, v1                           MOVE v4, v1
  // INVOKE_DIRECT v3, v4                  MOVE_OBJECT v7, v3
  // GOTO return_block           ==>       MOVE v8, v4
  // case_block_2:                         GOTO return_block
  // MOVE_OBJECT v5, v0                    case_block_2:
  // MOVE v6, v1                           MOVE_OBJECT v5, v0
  // INVOKE_DIRECT v5, v6                  MOVE v6, v1
  // GOTO return_block                     MOVE_OBJECT v7, v5
  //                                       MOVE v8, v6
  //                                       GOTO return_block
  //                                       return_block:
  //                                       INVOKE_DIRECT v7, v8
  //
  // Redundent moves should be cleaned up by opt passes like copy propagation.
  std::vector<reg_t> new_srcs;
  auto common_ctor_args = common_ctor->get_proto()->get_args();
  new_srcs.reserve(1 + common_ctor_args->size());
  // For "this" pointer which should be an object reference and is not a wide
  // register.
  new_srcs.push_back(dispatch_code->allocate_temp());
  for (auto arg_type : *common_ctor_args) {
    new_srcs.push_back(type::is_wide_type(arg_type)
                           ? dispatch_code->allocate_wide_temp()
                           : dispatch_code->allocate_temp());
  }

  for (const auto& invocation : invocations) {
    // For "this" pointer.
    dispatch_code->insert_before(invocation,
                                 (new IRInstruction(OPCODE_MOVE_OBJECT))
                                     ->set_src(0, invocation->insn->src(0))
                                     ->set_dest(new_srcs[0]));
    auto arg_it = common_ctor_args->begin();
    for (size_t i = 1; i < invocation->insn->srcs_size(); ++i, ++arg_it) {
      redex_assert(arg_it != common_ctor_args->end());
      auto mov = (new IRInstruction(opcode::move_opcode(*arg_it)))
                     ->set_src(0, invocation->insn->src(i))
                     ->set_dest(new_srcs[i]);
      dispatch_code->insert_before(invocation, mov);
    }
    dispatch_code->erase_and_dispose(invocation);
  }

  auto invoke = (new IRInstruction(OPCODE_INVOKE_DIRECT))
                    ->set_method(common_ctor)
                    ->set_srcs_size(new_srcs.size());
  for (size_t i = 0; i < new_srcs.size(); ++i) {
    invoke->set_src(i, new_srcs[i]);
  }
  dispatch_code->insert_before(return_block->get_first_insn(), invoke);
  dispatch_code->clear_cfg();
}

/**
 * Force inline dispatch entries if the subsequent inlining pass is not inclined
 * to do so. It is only needed when we want to make sure the entries in the
 * dispatch are indeed inlined in the final output.
 */
void ModelMethodMerger::inline_dispatch_entries(
    DexType* merger_type,
    DexMethod* dispatch,
    std::vector<std::pair<DexType*, DexMethod*>>&
        not_inlined_dispatch_entries) {
  // TODO:The clear_cfg flag is used to make sure that using editable cfg in
  // this function won't affect other parts which use non-editable cfg. Once the
  // pass is updated to editable cfg, this flag and check can be removed.
  bool clear_cfg = false;
  if (!dispatch->get_code()->editable_cfg_built()) {
    dispatch->get_code()->build_cfg();
    clear_cfg = true;
  }
  auto& dispatch_cfg = dispatch->get_code()->cfg();
  std::vector<std::pair<DexMethod*, IRInstruction*>> callsites;
  auto insns = InstructionIterable(dispatch_cfg);
  for (auto it = insns.begin(); it != insns.end(); ++it) {
    auto insn = it->insn;
    if (insn->opcode() != OPCODE_INVOKE_STATIC) {
      continue;
    }
    DexMethod* meth = resolve_method(insn->get_method(), MethodSearch::Static);
    if (meth != nullptr) {
      callsites.emplace_back(meth, insn);
    }
  }

  for (auto&& [callee, callsite] : callsites) {
    // TODO: same as clear_cfg flag.
    bool to_clear = false;
    if (!callee->get_code()->editable_cfg_built()) {
      callee->get_code()->build_cfg();
      to_clear = true;
    }
    bool inlined = inliner::inline_with_cfg(dispatch, callee, callsite,
                                            /* needs_receiver_cast */ nullptr,
                                            /* needs_init_class */ nullptr,
                                            dispatch_cfg.get_registers_size());
    if (to_clear) {
      callee->get_code()->clear_cfg();
    }
    if (!inlined) {
      TRACE(CLMG, 9, "inline dispatch entry %s failed!", SHOW(callee));
      not_inlined_dispatch_entries.emplace_back(merger_type, callee);
    }
  }
  TRACE(CLMG, 9, "inlined dispatch %s\n%s", SHOW(dispatch), SHOW(dispatch_cfg));
  if (clear_cfg) {
    dispatch->get_code()->clear_cfg();
  }
}

std::string ModelMethodMerger::get_method_signature_string(DexMethod* meth) {
  if (m_method_debug_map.count(meth) > 0) {
    auto orig_signature = m_method_debug_map.at(meth);
    TRACE(CLMG, 9, "Method debug map look up %s", orig_signature.c_str());
    return orig_signature;
  }

  return type_reference::get_method_signature(meth);
}

void ModelMethodMerger::merge_virtual_methods(
    const Scope& scope,
    DexType* super_type,
    DexType* target_type,
    DexField* type_tag_field,
    const std::vector<MergerType::VirtualMethod>& virt_methods,
    std::vector<std::pair<DexClass*, DexMethod*>>& dispatch_methods,
    std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee) {
  DexClass* target_cls = type_class(target_type);
  for (auto& virt_meth : virt_methods) {
    auto& meth_lst = virt_meth.overrides;
    always_assert(meth_lst.size());
    auto overridden_meth = virt_meth.base;
    auto front_meth = meth_lst.front();
    auto access = front_meth->get_access();
    auto dispatch_proto =
        DexProto::make_proto(front_meth->get_proto()->get_rtype(),
                             front_meth->get_proto()->get_args());

    // Make static
    MethodTypeTags type_tags;
    std::unordered_map<const DexType*, std::string> meth_signatures;
    for (auto m : meth_lst) {
      meth_signatures[m->get_class()] = get_method_signature_string(m);
      staticize_with_new_arg_head(m, target_type);
      type_tags[m] = m_type_tags->get_type_tag(m->get_class());
    }
    const auto name = front_meth->get_name()->str();

    // Create dispatch.
    dispatch::Spec spec{target_type,     dispatch::Type::VIRTUAL,
                        str_copy(name),  dispatch_proto,
                        access,          type_tag_field,
                        overridden_meth, m_max_num_dispatch_target,
                        boost::none,     m_model_spec.keep_debug_info};
    dispatch::DispatchMethod dispatch = create_dispatch_method(spec, meth_lst);
    for (const auto sub_dispatch : dispatch.sub_dispatches) {
      dispatch_methods.emplace_back(target_cls, sub_dispatch);
    }
    dispatch_methods.emplace_back(target_cls, dispatch.main_dispatch);
    for (const auto& m : meth_lst) {
      old_to_new_callee[m] = dispatch.main_dispatch;
    }
    // Populating method dedup map
    for (auto& type_to_sig : meth_signatures) {
      auto type = type_to_sig.first;
      auto map = std::make_pair(type_to_sig.second, dispatch.main_dispatch);
      m_method_dedup_map[type].push_back(map);
      TRACE(CLMG,
            9,
            " adding dedup map type %s %s -> %s",
            SHOW(type),
            type_to_sig.second.c_str(),
            SHOW(dispatch.main_dispatch));
    }
  }
}

void ModelMethodMerger::merge_ctors() {
  //////////////////////////////////////////
  // Collect type tags and call sites.
  //////////////////////////////////////////
  MethodTypeTags type_tags;
  MethodOrderedSet ctor_set;
  for (const auto& pair : m_merger_ctors) {
    auto target_type = const_cast<DexType*>(pair.first->type);
    auto target_cls = type_class(target_type);
    always_assert(target_cls);

    auto& ctors = pair.second;
    for (const auto m : ctors) {
      type_tags[m] = m_type_tags->get_type_tag(m->get_class());
    }
    ctor_set.insert(ctors.begin(), ctors.end());
  }

  bool pass_type_tag_param = m_model_spec.pass_type_tag_to_ctor();
  TRACE(CLMG, 5, "pass type tag param %d", pass_type_tag_param);
  //////////////////////////////////////////
  // Create dispatch and fixes
  //////////////////////////////////////////
  std::unordered_map<DexMethod*, DexMethod*> old_to_new_callee;
  for (const auto& pair : m_merger_ctors) {
    auto merger = pair.first;
    auto target_type = const_cast<DexType*>(merger->type);
    auto target_cls = type_class(target_type);
    auto type_tag_field = m_type_tag_fields.count(merger) > 0
                              ? m_type_tag_fields.at(merger)
                              : nullptr;
    // Group by proto.
    std::unordered_map<DexProto*, std::vector<DexMethod*>> proto_to_ctors;
    for (const auto m : pair.second) {
      proto_to_ctors[m->get_proto()].push_back(m);
    }
    always_assert(!proto_to_ctors.empty());
    TRACE(CLMG,
          4,
          " Merging ctors for %s with %zu different protos",
          SHOW(target_type),
          proto_to_ctors.size());
    std::unordered_set<DexMethod*> dispatches;
    for (const auto& ctors_pair : proto_to_ctors) {
      auto& ctors = ctors_pair.second;
      auto ctor_proto = ctors_pair.first;
      std::unordered_map<const DexType*, std::string> ctor_signatures;
      for (const auto ctor : ctors) {
        ctor_signatures[ctor->get_class()] =
            type_reference::get_method_signature(ctor);
        TRACE(CLMG, 9, "  converting ctor %s", SHOW(ctor));
        staticize_with_new_arg_head(ctor, target_type);
      }

      // Create dispatch.
      auto dispatch_arg_list = ctor_proto->get_args()->push_back(type::_int());
      auto dispatch_proto =
          pass_type_tag_param
              ? DexProto::make_proto(ctor_proto->get_rtype(), dispatch_arg_list)
              : ctor_proto;
      dispatch::Spec spec{
          target_type,
          (m_model_spec.generate_type_tag())
              ? dispatch::Type::CTOR_SAVE_TYPE_TAG_PARAM
              : dispatch::Type::CTOR,
          "<init>",
          dispatch_proto,
          ACC_PUBLIC | ACC_CONSTRUCTOR,
          type_tag_field,
          nullptr, // overridden_meth
          get_ctor_type_tag_param_idx(pass_type_tag_param, ctor_proto),
          m_model_spec.keep_debug_info};
      auto indices_to_callee = get_dedupped_indices_map(ctors);
      if (indices_to_callee.size() > 1) {
        always_assert_log(
            m_model_spec.has_type_tag(),
            "No type tag config cannot handle multiple dispatch targets!");
      }
      m_stats.m_num_ctor_dedupped += ctors.size() - indices_to_callee.size();
      auto dispatch =
          dispatch::create_ctor_or_static_dispatch(spec, indices_to_callee);
      for (const auto& m : ctors) {
        old_to_new_callee[m] = dispatch;
      }
      std::vector<std::pair<DexType*, DexMethod*>> not_inlined_ctors;
      type_class(target_type)->add_method(dispatch);
      // Inline entries
      inline_dispatch_entries(target_type, dispatch, not_inlined_ctors);
      sink_common_ctor_to_return_block(dispatch);
      auto mergeable_cls = type_class(ctors.front()->get_class());
      always_assert(mergeable_cls->get_super_class() ==
                    target_cls->get_super_class());

      // Remove mergeable ctors
      // The original mergeable ctors have been converted to static and won't
      // pass VFY.
      redex_assert(not_inlined_ctors.empty());
      for (const auto ctor : ctors) {
        auto cls = type_class(ctor->get_class());
        cls->remove_method(ctor);
      }

      // Populating method dedup map
      for (auto& type_to_sig : ctor_signatures) {
        auto type = type_to_sig.first;
        auto map = std::make_pair(type_to_sig.second, dispatch);
        m_method_dedup_map[type].push_back(map);
        TRACE(CLMG,
              9,
              " adding dedup map type %s %s -> %s",
              SHOW(type),
              type_to_sig.second.c_str(),
              SHOW(dispatch));
      }

      dispatches.emplace(dispatch);
    }
    // Update mergeable ctor map
    for (auto type : pair.first->mergeables) {
      for (auto dispatch : dispatches) {
        m_mergeable_to_merger_ctor[type] = dispatch;
      }
    }
  }
  //////////////////////////////////////////
  // Update call sites
  //////////////////////////////////////////
  auto call_sites = method_reference::collect_call_refs(m_scope, ctor_set);
  update_call_refs(call_sites, type_tags, old_to_new_callee,
                   pass_type_tag_param);
}

void ModelMethodMerger::dedup_non_ctor_non_virt_methods() {
  for (auto merger : m_mergers) {
    auto merger_type = const_cast<DexType*>(merger->type);
    std::vector<DexMethod*> to_dedup;
    // Add non_ctors and non_vmethods
    auto& non_ctors = m_merger_non_ctors.at(merger);
    to_dedup.insert(to_dedup.end(), non_ctors.begin(), non_ctors.end());
    auto& non_vmethods = m_merger_non_vmethods.at(merger);
    to_dedup.insert(to_dedup.end(), non_vmethods.begin(), non_vmethods.end());

    // Lift constants
    if (m_model_spec.process_method_meta) {
      ConstantLifting const_lift;
      std::vector<DexMethod*> annotated;
      for (const auto m : to_dedup) {
        if (const_lift.is_applicable_to_constant_lifting(m)) {
          annotated.push_back(m);
        }
      }
      TRACE(CLMG, 8, "const lift: start %zu", annotated.size());
      auto stub_methods = const_lift.lift_constants_from(
          m_scope, m_type_tags, annotated, CONST_LIFT_STUB_THRESHOLD);
      to_dedup.insert(to_dedup.end(), stub_methods.begin(), stub_methods.end());
      m_stats.m_num_const_lifted_methods +=
          const_lift.get_num_const_lifted_methods();
      for (auto stub : stub_methods) {
        if (stub->is_virtual()) {
          non_vmethods.push_back(stub);
        } else {
          non_ctors.push_back(stub);
        }
      }
    }

    // Dedup non_ctors & non_vmethods
    std::vector<DexMethod*> replacements;
    std::unordered_map<DexMethod*, MethodOrderedSet> new_to_old;
    auto new_to_old_optional =
        boost::optional<std::unordered_map<DexMethod*, MethodOrderedSet>>(
            new_to_old);
    m_stats.m_num_static_non_virt_dedupped += method_dedup::dedup_methods(
        m_scope, to_dedup, m_model_spec.dedup_fill_in_stack_trace, replacements,
        new_to_old_optional);

    // Relocate the remainders.
    std::set<DexMethod*, dexmethods_comparator> to_relocate(
        replacements.begin(), replacements.end());
    // Add to methods stats
    if (traceEnabled(CLMG, 8)) {
      m_method_stats.add(to_relocate);
    }
    for (auto m : to_relocate) {
      auto sig = get_method_signature_string(m);
      auto map = std::make_pair(sig, m);
      m_method_dedup_map[m->get_class()].push_back(map);
      TRACE(CLMG,
            9,
            "dedup: adding dedup map type %s %s -> %s",
            SHOW(m->get_class()),
            SHOW(m),
            SHOW(merger_type));

      TRACE(CLMG, 8, "dedup: moving static|non_virt method %s", SHOW(m));
      relocate_method(m, merger_type);
    }

    // Update method dedup map
    for (auto& pair : new_to_old) {
      auto old_list = pair.second;
      for (auto old_meth : old_list) {
        auto type = old_meth->get_class();
        if (m_mergeable_to_merger_ctor.count(type) == 0) {
          continue;
        }
        auto sig = get_method_signature_string(old_meth);
        auto map = std::make_pair(sig, pair.first);
        m_method_dedup_map[type].push_back(map);
        TRACE(CLMG,
              9,
              "dedup: adding dedup map type %s %s -> %s",
              SHOW(type),
              SHOW(old_meth),
              SHOW(pair.first));
      }
    }

    // Clean up remainders, update the non_ctors and non_vmethods.
    auto should_erase = [&merger_type, this](DexMethod* m) {
      auto owner = m->get_class();
      if (owner == merger_type) {
        return false;
      }
      TRACE(CLMG, 9, "dedup: removing %s", SHOW(m));
      always_assert(m_mergeable_to_merger_ctor.count(owner));
      auto cls = type_class(owner);
      cls->remove_method(m);
      DexMethod::erase_method(m);
      DexMethod::delete_method(m);
      return true;
    };
    int before = non_ctors.size() + non_vmethods.size();
    non_ctors.erase(
        std::remove_if(non_ctors.begin(), non_ctors.end(), should_erase),
        non_ctors.end());
    non_vmethods.erase(
        std::remove_if(non_vmethods.begin(), non_vmethods.end(), should_erase),
        non_vmethods.end());
    TRACE(CLMG,
          8,
          "dedup: clean up static|non_virt remainders %zu",
          before - non_ctors.size() - non_vmethods.size());
  }
}

void ModelMethodMerger::merge_virt_itf_methods() {
  std::vector<std::pair<DexClass*, DexMethod*>> dispatch_methods;
  std::unordered_map<DexMethod*, DexMethod*> old_to_new_callee;

  for (auto merger : m_mergers) {
    auto merger_type = const_cast<DexType*>(merger->type);
    auto merger_cls = type_class(merger_type);
    always_assert(merger_cls);
    auto super_type = merger_cls->get_super_class();
    auto type_tag_field = m_type_tag_fields.count(merger) > 0
                              ? m_type_tag_fields.at(merger)
                              : nullptr;
    std::vector<MergerType::VirtualMethod> virt_methods;

    for (auto& vm_lst : merger->vmethods) {
      virt_methods.emplace_back(vm_lst);
    }
    for (auto& im : merger->intfs_methods) {
      virt_methods.emplace_back(im.overridden_meth, im.methods);
    }

    merge_virtual_methods(m_scope,
                          super_type,
                          merger_type,
                          type_tag_field,
                          virt_methods,
                          dispatch_methods,
                          old_to_new_callee);
  }

  method_reference::update_call_refs_simple(m_scope, old_to_new_callee);
  // Adding dispatch after updating callsites to avoid patching callsites within
  // the dispatch switch itself.
  std::vector<std::pair<DexType*, DexMethod*>> not_inlined_dispatch_entries;
  for (auto& pair : dispatch_methods) {
    auto merger_cls = pair.first;
    auto dispatch = pair.second;
    merger_cls->add_method(dispatch);
    inline_dispatch_entries(merger_cls->get_type(), dispatch,
                            not_inlined_dispatch_entries);
  }
  // Only relocate dispatch entries that for what whatever reason were not
  // inlined. They are however still referenced by the dispatch. What's left on
  // the merged classes will be purged later.
  for (const auto& pair : not_inlined_dispatch_entries) {
    auto merger_type = pair.first;
    auto not_inlined = pair.second;
    relocate_method(not_inlined, merger_type);
  }
}

} // namespace class_merging
