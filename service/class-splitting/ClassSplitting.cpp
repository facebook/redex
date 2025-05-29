/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>

#include "ApiLevelChecker.h"
#include "ClassSplitting.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "Mutators.h"
#include "PluginRegistry.h"
#include "Resolver.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

namespace class_splitting {

const int TRAMPOLINE_THRESHOLD_SIZE = 32;

void update_coldstart_classes_order(
    ConfigFiles& conf,
    PassManager& mgr,
    const UnorderedSet<DexType*>& coldstart_types,
    const std::vector<std::string>& previously_relocated_types,
    bool log /* = true */) {
  const auto& coldstart_classes = conf.get_coldstart_classes();

  UnorderedMap<std::string, std::string> replacement;
  for (const auto& str : previously_relocated_types) {
    auto initial_type = str.substr(0, str.size() - 11) + ";";

    auto type = DexType::get_type(initial_type);
    if (type == nullptr) {
      TRACE(CS, 2,
            "[class splitting] Cannot find previously relocated type %s in "
            "cold-start classes",
            initial_type.c_str());
      mgr.incr_metric("num_missing_initial_types", 1);
      continue;
    }

    if (!coldstart_types.count(type)) {
      replacement[str] = initial_type;
    }
  }

  if (!replacement.empty()) {
    std::vector<std::string> new_coldstart_classes(coldstart_classes.size());

    for (const auto& str : coldstart_classes) {
      if (replacement.count(str)) {
        new_coldstart_classes.push_back(replacement[str]);
      } else {
        new_coldstart_classes.push_back(str);
      }
    }

    conf.update_coldstart_classes(std::move(new_coldstart_classes));
  }

  if (log) {
    mgr.set_metric("num_coldstart_classes_updated", replacement.size());
  }
}

ClassSplitter::ClassSplitter(
    const ClassSplittingConfig& config,
    PassManager& mgr,
    const UnorderedSet<DexMethod*>& sufficiently_popular_methods,
    const UnorderedSet<DexMethod*>& insufficiently_popular_methods)
    : m_config(config),
      m_mgr(mgr),
      m_sufficiently_popular_methods(sufficiently_popular_methods),
      m_insufficiently_popular_methods(insufficiently_popular_methods) {
  // Instead of changing visibility as we split, blocking other work on the
  // critical path, we do it all in parallel at the end.
  m_delayed_visibility_changes = std::make_unique<VisibilityChanges>();
}

void ClassSplitter::configure(const Scope& scope) {
  if (m_config.relocate_non_true_virtual_methods) {
    m_non_true_virtual_methods = method_override_graph::get_non_true_virtuals(
        *method_override_graph::build_graph(scope), scope);
  }
}

DexClass* ClassSplitter::create_target_class(
    const std::string& target_type_name) {
  DexType* target_type = DexType::make_type(target_type_name);
  ++m_stats.relocation_classes;
  ClassCreator cc(target_type);
  cc.set_access(ACC_PUBLIC | ACC_FINAL);
  cc.set_super(type::java_lang_Object());
  auto target_cls = cc.create();
  target_cls->rstate.set_generated();
  target_cls->set_deobfuscated_name(target_type_name);
  return target_cls;
}

size_t ClassSplitter::get_trampoline_method_cost(DexMethod* method) {
  // Maybe this can be calculated? Here goes the size of code for pushing
  // parameters, making the call, adding refs, etc For now, empirically derive
  // the best value.
  return TRAMPOLINE_THRESHOLD_SIZE;
}

DexMethod* ClassSplitter::create_trampoline_method(DexMethod* method,
                                                   DexClass* target_cls,
                                                   uint32_t api_level) {
  std::string name = method->get_name()->str_copy();
  // We are merging two "namespaces" here, so we make it clear what kind of
  // method a trampoline came from. We don't support combining target classes
  // by api-level here, as we'd have to do more uniquing.
  always_assert(!m_config.combine_target_classes_by_api_level);
  if (method->is_virtual()) {
    name += "$vtramp";
  } else {
    name += "$dtramp";
  }
  DexTypeList::ContainerType arg_types;
  if (!is_static(method)) {
    arg_types.push_back(method->get_class());
  }
  for (auto t : *method->get_proto()->get_args()) {
    arg_types.push_back(const_cast<DexType*>(t));
  }
  auto type_list = DexTypeList::make_type_list(std::move(arg_types));
  auto proto =
      DexProto::make_proto(method->get_proto()->get_rtype(), type_list);
  auto trampoline_target_method =
      DexMethod::make_method(target_cls->get_type(),
                             DexString::make_string(name), proto)
          ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  trampoline_target_method->set_deobfuscated_name(
      show_deobfuscated(trampoline_target_method));
  trampoline_target_method->rstate.set_api_level(api_level);
  target_cls->add_method(trampoline_target_method);
  return trampoline_target_method;
}

bool ClassSplitter::has_source_block_positive_val(DexMethod* method) {
  for (auto& mie : *method->get_code()) {
    if (mie.type == MFLOW_SOURCE_BLOCK &&
        source_blocks::has_source_block_positive_val(mie.src_block.get())) {
      return true;
    }
  }
  return false;
}

void ClassSplitter::prepare(const DexClass* cls,
                            std::vector<DexMethodRef*>* mrefs,
                            std::vector<DexType*>* trefs) {
  // Bail out if we just cannot or should not relocate methods of this class.
  if (!can_relocate(cls)) {
    return;
  }
  auto cls_has_problematic_clinit = method::clinit_may_have_side_effects(
      cls, /* allow_benign_method_invocations */ false);

  SplitClass& sc = m_split_classes[cls];
  always_assert(sc.relocatable_methods.empty());
  auto process_method = [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }
    auto& cfg = method->get_code()->cfg();
    if (get_trampoline_method_cost(method) >= cfg.estimate_code_units()) {
      m_stats.method_size_too_small++;
      return;
    }
    if (m_sufficiently_popular_methods.count(method)) {
      return;
    }
    if (m_config.profile_only &&
        !m_insufficiently_popular_methods.count(method)) {
      return;
    }
    if (m_config.source_blocks && has_source_block_positive_val(method)) {
      return;
    }

    bool requires_trampoline{false};
    if (!can_relocate(cls_has_problematic_clinit, method, /* log */ true,
                      &requires_trampoline)) {
      return;
    }
    if (requires_trampoline && !m_config.trampolines) {
      return;
    }
    DexClass* target_cls;
    int api_level = api::LevelChecker::get_method_level(method);
    if (m_config.combine_target_classes_by_api_level) {
      TargetClassInfo& target_class_info =
          m_target_classes_by_api_level[api_level];
      if (target_class_info.target_cls == nullptr ||
          (target_class_info.last_source_cls != cls &&
           target_class_info.size >=
               m_config.relocated_methods_per_target_class)) {
        std::stringstream ss;
        ss << "Lredex/$Relocated" << std::to_string(m_next_target_class_index++)
           << "ApiLevel" << std::to_string(api_level) << ";";
        target_cls = create_target_class(ss.str());
        target_class_info.target_cls = target_cls;
        target_class_info.last_source_cls = cls;
        target_class_info.size = 0;
      } else {
        target_cls = target_class_info.target_cls;
      }
      ++target_class_info.size;
    } else {
      auto source_cls = method->get_class();
      auto it = m_target_classes_by_source_classes.find(source_cls);
      if (it != m_target_classes_by_source_classes.end()) {
        target_cls = it->second;
      } else {
        auto source_name = source_cls->str();
        target_cls =
            create_target_class(source_name.substr(0, source_name.size() - 1) +
                                CLASS_SPLITTING_RELOCATED_SUFFIX_SEMI);
        m_target_classes_by_source_classes.emplace(source_cls, target_cls);
      }
    }
    DexMethod* trampoline_target_method = nullptr;
    if (requires_trampoline) {
      trampoline_target_method =
          create_trampoline_method(method, target_cls, api_level);
    }
    sc.relocatable_methods.insert(
        {method, {target_cls, trampoline_target_method, api_level}});
    if (trefs != nullptr) {
      trefs->push_back(target_cls->get_type());
    }
    if (mrefs != nullptr && trampoline_target_method != nullptr) {
      mrefs->push_back(trampoline_target_method);
    }
    TRACE(CS, 4, "[class splitting] Method {%s} will be relocated to {%s}",
          SHOW(method), SHOW(target_cls));

    if (m_instrumentation_callback.target<void (*)(DexMethod*)>() != nullptr) {
      m_instrumentation_callback(method);
    }
  };
  auto& dmethods = cls->get_dmethods();
  std::for_each(dmethods.begin(), dmethods.end(), process_method);
  auto& vmethods = cls->get_vmethods();
  std::for_each(vmethods.begin(), vmethods.end(), process_method);
}

DexClasses ClassSplitter::additional_classes(const DexClasses& classes) {
  // Here, we are going to do the final determination of what to relocate ---
  // After checking if things still look as they did before, and no other
  // interdex pass or feature tinkered with the relocatability...
  // The actual relocation will happen in cleanup, so that we don't interfere
  // with earlier InterDex cleanups that still expect the code to be in their
  // original places.

  DexClasses target_classes;
  UnorderedSet<const DexClass*> target_classes_set;
  size_t relocated_methods = 0;
  // We iterate over the actually added set of classes.
  for (DexClass* cls : classes) {
    auto split_classes_it = m_split_classes.find(cls);
    if (split_classes_it == m_split_classes.end()) {
      continue;
    }
    const SplitClass& sc = split_classes_it->second;
    auto& dmethods = cls->get_dmethods();
    auto& vmethods = cls->get_vmethods();
    if (!can_relocate(cls)) {
      TRACE(CS,
            4,
            "[class splitting] Class earlier identified as relocatable is "
            "no longer relocatable: {%s}",
            SHOW(cls));
      continue;
    }
    auto cls_has_problematic_clinit = method::clinit_may_have_side_effects(
        cls, /* allow_benign_method_invocations */ false);
    std::vector<DexMethod*> methods_to_relocate;
    // We iterate over the actually existing set of methods at this time
    // (other InterDex plug-ins might have added or removed or relocated
    // methods).
    auto process_method = [&](DexMethod* method) {
      if (!method->get_code()) {
        return;
      }
      if (m_sufficiently_popular_methods.count(method)) {
        m_stats.popular_methods++;
        return;
      }
      if (m_config.profile_only &&
          !m_insufficiently_popular_methods.count(method)) {
        m_stats.non_relocated_methods++;
        return;
      }
      if (m_config.source_blocks && has_source_block_positive_val(method)) {
        m_stats.source_block_positive_vals++;
        return;
      }

      auto it = sc.relocatable_methods.find(method);
      if (it == sc.relocatable_methods.end()) {
        m_stats.non_relocated_methods++;
        return;
      }
      const RelocatableMethodInfo& method_info = it->second;
      bool requires_trampoline{false};
      if (!can_relocate(cls_has_problematic_clinit, method, /* log */ false,
                        &requires_trampoline)) {
        TRACE(CS,
              4,
              "[class splitting] Method earlier identified as relocatable is "
              "no longer relocatable: {%s}",
              SHOW(method));
        m_stats.non_relocated_methods++;
        return;
      }
      if (requires_trampoline &&
          method_info.trampoline_target_method == nullptr) {
        TRACE(CS,
              4,
              "[class splitting] Method earlier identified as not requiring "
              "a trampoline now requires a trampoline: {%s}",
              SHOW(method));
        m_stats.non_relocated_methods++;
        return;
      }
      int api_level = api::LevelChecker::get_method_level(method);
      if (api_level != method_info.api_level) {
        TRACE(CS, 4,
              "[class splitting] Method {%s} api level changed to {%d} from "
              "{%d}.",
              SHOW(method), api_level, method_info.api_level);
        m_stats.non_relocated_methods++;
        return;
      }

      methods_to_relocate.push_back(method);
    };
    std::for_each(dmethods.begin(), dmethods.end(), process_method);
    std::for_each(vmethods.begin(), vmethods.end(), process_method);

    for (DexMethod* method : methods_to_relocate) {
      const RelocatableMethodInfo& method_info =
          sc.relocatable_methods.at(method);

      if (method_info.trampoline_target_method != nullptr) {
        m_methods_to_trampoline.emplace_back(
            method, method_info.trampoline_target_method);
      } else {
        m_methods_to_relocate.emplace_back(method, method_info.target_cls);
      }
      ++relocated_methods;
      if (is_static(method)) {
        ++m_stats.relocated_static_methods;
      } else if (!method->is_virtual()) {
        ++m_stats.relocated_non_static_direct_methods;
      } else if (m_non_true_virtual_methods.count_unsafe(method)) {
        ++m_stats.relocated_non_true_virtual_methods;
      } else {
        ++m_stats.relocated_true_virtual_methods;
      }

      TRACE(CS, 3, "[class splitting] Method {%s} relocated to {%s}",
            SHOW(method), SHOW(method_info.target_cls));

      if (target_classes_set.insert(method_info.target_cls).second) {
        target_classes.push_back(method_info.target_cls);
      }
    }
  }

  TRACE(CS, 2,
        "[class splitting] Relocated {%zu} methods to {%zu} target classes "
        "in this dex.",
        relocated_methods, target_classes.size());

  m_target_classes_by_api_level.clear();
  m_split_classes.clear();
  return target_classes;
}

void ClassSplitter::materialize_trampoline_code(DexMethod* source,
                                                DexMethod* target) {
  // "source" is the original method, still in its original place.
  // "target" is the new trampoline target method, somewhere far away
  target->set_code(
      std::make_unique<IRCode>(std::make_unique<cfg::ControlFlowGraph>()));
  auto code = source->get_code();
  auto& cfg = code->cfg();
  auto& target_cfg = target->get_code()->cfg();
  cfg.deep_copy(&target_cfg);
  code->clear_cfg();
  source->set_code(
      std::make_unique<IRCode>(std::make_unique<cfg::ControlFlowGraph>()));
  // Create a new block containing all the load instructions.
  cfg = source->get_code()->cfg();
  cfg::Block* new_block = cfg.create_block();
  auto invoke_insn = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_insn->set_method(target);
  auto proto = target->get_proto();
  auto* type_list = proto->get_args();
  invoke_insn->set_srcs_size(type_list->size());
  for (size_t i = 0; i < type_list->size(); i++) {
    auto t = type_list->at(i);
    IRInstruction* load_param_insn;
    if (type::is_wide_type(t)) {
      load_param_insn = new IRInstruction(IOPCODE_LOAD_PARAM_WIDE);
      load_param_insn->set_dest(cfg.allocate_wide_temp());
    } else {
      load_param_insn = new IRInstruction(
          type::is_object(t) ? IOPCODE_LOAD_PARAM_OBJECT : IOPCODE_LOAD_PARAM);
      load_param_insn->set_dest(cfg.allocate_temp());
    }
    new_block->push_back(load_param_insn);
    invoke_insn->set_src(i, load_param_insn->dest());
  }
  new_block->push_back(invoke_insn);
  IRInstruction* return_insn;
  if (proto->get_rtype() != type::_void()) {
    auto t = proto->get_rtype();
    IRInstruction* move_result_insn;
    if (type::is_wide_type(t)) {
      move_result_insn = new IRInstruction(OPCODE_MOVE_RESULT_WIDE);
      move_result_insn->set_dest(cfg.allocate_wide_temp());
      return_insn = new IRInstruction(OPCODE_RETURN_WIDE);
    } else {
      move_result_insn = new IRInstruction(
          type::is_object(t) ? OPCODE_MOVE_RESULT_OBJECT : OPCODE_MOVE_RESULT);
      move_result_insn->set_dest(cfg.allocate_temp());
      return_insn = new IRInstruction(type::is_object(t) ? OPCODE_RETURN_OBJECT
                                                         : OPCODE_RETURN);
    }
    new_block->push_back(move_result_insn);
    return_insn->set_src(0, move_result_insn->dest());
  } else {
    return_insn = new IRInstruction(OPCODE_RETURN_VOID);
  }
  new_block->push_back(return_insn);
  TRACE(CS, 5, "[class splitting] New body for {%s}: \n%s", SHOW(source),
        SHOW(cfg));
  change_visibility(target);
}

void ClassSplitter::cleanup(const Scope& final_scope) {
  // Here we do the actual relocation.

  // Part 1: Upgrade non-static invokes to static invokes
  UnorderedSet<DexMethod*> methods_to_staticize;
  for (auto& p : m_methods_to_relocate) {
    DexMethod* method = p.first;
    if (!is_static(method)) {
      methods_to_staticize.insert(method);
    }
  }

  // We now rewrite all invoke-instructions as needed to reflect the fact that
  // we made some methods static as part of the relocation effort.
  UnorderedMap<IROpcode, std::atomic<size_t>, boost::hash<IROpcode>>
      rewritten_invokes;
  for (IROpcode op :
       {OPCODE_INVOKE_DIRECT, OPCODE_INVOKE_VIRTUAL, OPCODE_INVOKE_SUPER}) {
    rewritten_invokes[op] = 0;
  }
  walk::parallel::opcodes(
      final_scope, [](DexMethod*) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        auto op = insn->opcode();
        switch (op) {
        case OPCODE_INVOKE_DIRECT:
        case OPCODE_INVOKE_VIRTUAL:
        case OPCODE_INVOKE_SUPER: {
          auto resolved_method = resolve_method(insn->get_method(),
                                                opcode_to_search(insn), method);
          if (resolved_method && methods_to_staticize.count(resolved_method)) {
            insn->set_opcode(OPCODE_INVOKE_STATIC);
            insn->set_method(resolved_method);
            rewritten_invokes.at(op)++;
          }
          break;
        }
        case OPCODE_INVOKE_INTERFACE:
        case OPCODE_INVOKE_STATIC: {
          auto resolved_method = resolve_method(insn->get_method(),
                                                opcode_to_search(insn), method);
          always_assert(!resolved_method ||
                        !methods_to_staticize.count(resolved_method));
          break;
        }
        default:
          break;
        }
      });
  TRACE(CS, 2,
        "[class splitting] Rewrote {%zu} direct, {%zu} virtual, {%zu} super "
        "invokes.",
        (size_t)rewritten_invokes.at(OPCODE_INVOKE_DIRECT),
        (size_t)rewritten_invokes.at(OPCODE_INVOKE_VIRTUAL),
        (size_t)rewritten_invokes.at(OPCODE_INVOKE_SUPER));

  m_mgr.incr_metric(METRIC_STATICIZED_METHODS, methods_to_staticize.size());
  for (auto& p : UnorderedIterable(rewritten_invokes)) {
    m_mgr.incr_metric(std::string(METRIC_REWRITTEN_INVOKES) + SHOW(p.first),
                      (size_t)p.second);
  }

  // Part 2: Actually relocate and make static
  for (auto& p : m_methods_to_relocate) {
    DexMethod* method = p.first;
    DexClass* target_cls = p.second;
    set_public(method);
    if (!is_static(method)) {
      mutators::make_static(method);
    }
    change_visibility(method, target_cls->get_type());
    relocate_method(method, target_cls->get_type());
  }
  TRACE(CS, 2, "[class splitting] Made {%zu} methods static.",
        methods_to_staticize.size());

  // Part 3: Materialize trampolines
  for (auto& p : m_methods_to_trampoline) {
    materialize_trampoline_code(p.first, p.second);
  }

  delayed_visibility_changes_apply();
  delayed_invoke_direct_to_static(final_scope);
  m_delayed_make_static.clear();

  m_mgr.incr_metric(METRIC_RELOCATION_CLASSES, m_stats.relocation_classes);
  m_mgr.incr_metric(METRIC_RELOCATED_STATIC_METHODS,
                    m_stats.relocated_static_methods);
  m_mgr.incr_metric(METRIC_RELOCATED_NON_STATIC_DIRECT_METHODS,
                    m_stats.relocated_non_static_direct_methods);
  m_mgr.incr_metric(METRIC_RELOCATED_NON_TRUE_VIRTUAL_METHODS,
                    m_stats.relocated_non_true_virtual_methods);
  m_mgr.incr_metric(METRIC_RELOCATED_TRUE_VIRTUAL_METHODS,
                    m_stats.relocated_true_virtual_methods);
  m_mgr.incr_metric(METRIC_NON_RELOCATED_METHODS,
                    m_stats.non_relocated_methods);
  m_mgr.incr_metric(METRIC_POPULAR_METHODS, m_stats.popular_methods);
  m_mgr.incr_metric(METRIC_SOURCE_BLOCKS_POSITIVE_VALS,
                    m_stats.source_block_positive_vals);
  m_mgr.incr_metric(METRIC_RELOCATED_METHODS, m_methods_to_relocate.size());
  m_mgr.incr_metric(METRIC_TRAMPOLINES, m_methods_to_trampoline.size());
  m_mgr.incr_metric(METRIC_TOO_SMALL_METHODS, m_stats.method_size_too_small);

  TRACE(CS, 2,
        "[class splitting] Relocated {%zu} methods and created {%zu} "
        "trampolines",
        m_methods_to_relocate.size(), m_methods_to_trampoline.size());
  TRACE(CS, 2,
        "[class splitting] Encountered {%zu} popular and {%zu} non-relocated "
        "methods.",
        m_stats.popular_methods, m_stats.non_relocated_methods);

  // Releasing memory
  m_target_classes_by_api_level.clear();
  m_target_classes_by_source_classes.clear();
  m_split_classes.clear();
  m_methods_to_relocate.clear();
  m_methods_to_trampoline.clear();
}

bool ClassSplitter::matches(const std::string& name, const std::string& v) {
  return name.find(v) != std::string::npos;
}

bool ClassSplitter::can_relocate(const DexClass* cls) {
  return !cls->is_external() && !cls->rstate.is_generated() &&
         std::find_if(m_config.blocklist_types.begin(),
                      m_config.blocklist_types.end(),
                      [&](const std::string& v) {
                        return matches(cls->c_str(), v);
                      }) == m_config.blocklist_types.end();
}

bool ClassSplitter::has_unresolvable_or_external_field_ref(const DexMethod* m) {
  auto code = const_cast<DexMethod*>(m)->get_code();
  always_assert(code);
  auto& cfg = code->cfg();
  for (const auto& mie : cfg::InstructionIterable(cfg)) {
    auto insn = mie.insn;
    if (insn->has_field()) {
      auto field = resolve_field(insn->get_field(),
                                 opcode::is_an_sfield_op(insn->opcode())
                                     ? FieldSearch::Static
                                     : FieldSearch::Instance);

      if (!field || (!is_public(field) && field->is_external())) {
        return true;
      }
    }
  }

  return false;
}

bool ClassSplitter::can_relocate(bool cls_has_problematic_clinit,
                                 const DexMethod* m,
                                 bool log,
                                 bool* requires_trampoline) {
  *requires_trampoline = false;
  if (!m->is_concrete() || m->is_external() || !m->get_code()) {
    return false;
  }
  if (!can_rename(m)) {
    if (log) {
      m_mgr.incr_metric("num_class_splitting_limitation_cannot_rename", 1);
    }
    *requires_trampoline = true;
  }
  if (root(m)) {
    if (log) {
      m_mgr.incr_metric("num_class_splitting_limitation_root", 1);
    }
    *requires_trampoline = true;
  }
  if (m->rstate.no_optimizations()) {
    if (log) {
      m_mgr.incr_metric("num_class_splitting_limitation_no_optimizations", 1);
    }
    return false;
  }
  if (!gather_invoked_methods_that_prevent_relocation(m)) {
    if (log) {
      m_mgr.incr_metric(
          "num_class_splitting_limitation_invoked_methods_prevent_relocation",
          1);
    }
    return false;
  }
  if (has_unresolvable_or_external_field_ref(m)) {
    if (log) {
      m_mgr.incr_metric(
          "num_class_splitting_limitation_has_unresolvable_or_external_field_"
          "ref",
          1);
    }
    return false;
  }
  if (!method::no_invoke_super(*m->get_code())) {
    if (log) {
      m_mgr.incr_metric("num_class_splitting_limitation_invoke_super", 1);
    }
    return false;
  }
  if (m->rstate.is_generated()) {
    if (log) {
      m_mgr.incr_metric("num_class_splitting_limitation_generated", 1);
    }
    return false;
  }

  if (is_static(m)) {
    if (!m_config.relocate_static_methods) {
      return false;
    }
    if (cls_has_problematic_clinit) {
      if (log) {
        m_mgr.incr_metric(
            "num_class_splitting_limitation_static_method_declaring_class_"
            "has_clinit",
            1);
      }
      *requires_trampoline = true;
    }
    if (method::is_clinit(m)) {
      if (log) {
        m_mgr.incr_metric(
            "num_class_splitting_limitation_static_method_is_clinit", 1);
      }
      // TODO: Could be done with trampolines if we remove "final" flag from
      // fields
      return false;
    }
  } else if (!m->is_virtual()) {
    if (!m_config.relocate_non_static_direct_methods) {
      return false;
    }
    if (method::is_init(m)) {
      if (log) {
        m_mgr.incr_metric("num_class_splitting_limitation_method_is_init", 1);
      }
      // TODO: Could be done with trampolines if we remove "final" flag from
      // fields, and carefully deal with super-init calls.
      return false;
    }
  } else if (m_non_true_virtual_methods.count_unsafe(
                 const_cast<DexMethod*>(m))) {
    if (!m_config.relocate_non_true_virtual_methods) {
      return false;
    }
  } else {
    if (!m_config.relocate_true_virtual_methods) {
      return false;
    }
    *requires_trampoline = true;
  }
  if (*requires_trampoline && m->get_code()->estimate_code_units() <
                                  m_config.trampoline_size_threshold) {
    if (log) {
      m_mgr.incr_metric("num_class_splitting_trampoline_size_threshold_not_met",
                        1);
    }
    return false;
  }
  VisibilityChanges visibility_changes = get_visibility_changes(m);
  if (!visibility_changes.empty()) {
    m_delayed_visibility_changes->insert(visibility_changes);
  }
  return true;
}

void ClassSplitter::delayed_visibility_changes_apply() {
  m_delayed_visibility_changes->apply();
  // any method that was just made public and isn't virtual or a constructor or
  // static must be made static
  for (auto method : UnorderedIterable(m_delayed_visibility_changes->methods)) {
    always_assert(is_public(method));
    if (!method->is_virtual() && !method::is_init(method) &&
        !is_static(method)) {
      always_assert(can_rename(method));
      always_assert(method->is_concrete());
      m_delayed_make_static.insert(method);
    }
  }
}

void ClassSplitter::delayed_invoke_direct_to_static(const Scope& final_scope) {
  if (m_delayed_make_static.empty()) {
    return;
  }
  // We sort the methods here because make_static renames methods on
  // collision, and which collisions occur is order-dependent. E.g. if we have
  // the following methods in m_delayed_make_static:
  //
  //   Foo Foo::bar()
  //   Foo Foo::bar(Foo f)
  //
  // making Foo::bar() static first would make it collide with Foo::bar(Foo
  // f), causing it to get renamed to bar$redex0(). But if Foo::bar(Foo f)
  // gets static-ified first, it becomes Foo::bar(Foo f, Foo f), so when bar()
  // gets made static later there is no collision. So in the interest of
  // having reproducible binaries, we sort the methods first.
  //
  // Also, we didn't use an std::set keyed by method signature here because
  // make_static is mutating the signatures. The tree that implements the set
  // would have to be rebalanced after the mutations.
  auto methods =
      unordered_to_ordered(m_delayed_make_static, compare_dexmethods);
  for (auto method : methods) {
    TRACE(MMINL, 6, "making %s static", method->get_name()->c_str());
    mutators::make_static(method);
  }
  walk::parallel::opcodes(
      final_scope, [](DexMethod* meth) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        auto op = insn->opcode();
        if (op == OPCODE_INVOKE_DIRECT) {
          auto m = insn->get_method()->as_def();
          if (m && m_delayed_make_static.count(m)) {
            insn->set_opcode(OPCODE_INVOKE_STATIC);
          }
        }
      });
  m_delayed_make_static.clear();
}

void ClassSplitter::set_instrumentation_callback(
    std::function<void(DexMethod*)>&& callback) {
  m_instrumentation_callback = std::move(callback);
}
} // namespace class_splitting
