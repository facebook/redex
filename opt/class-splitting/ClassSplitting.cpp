/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass splits out methods that are not frequently called (see
 * method_profiles_appear_percent_threshold for the frequent threashold) from
 * the cold-start dexes.
 *
 * The approach here is a new interdex plugin (with the possibility of running
 * it outside InterDex as well). This enables:
 * - only treating classes that end up in the non-primary cold-start dexes;
 * - accounting for extra classes, which is important to determine when a dex
 *   is full.
 *
 * Relocated methods are moved into new special classes. Each class is filled
 * with up to a configurable number of methods; only when a class is full,
 * another one is created. Separate classes might be created for distinct
 * required api levels.
 */

#include "ClassSplitting.h"

#include <algorithm>
#include <boost/functional/hash.hpp>
#include <vector>

#include "ApiLevelChecker.h"
#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPass.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "Mutators.h"
#include "PassManager.h"
#include "PluginRegistry.h"
#include "Resolver.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_STATICIZED_METHODS =
    "num_class_splitting_staticized_methods";
constexpr const char* METRIC_REWRITTEN_INVOKES =
    "num_class_splitting_rewritten_";
constexpr const char* METRIC_RELOCATION_CLASSES =
    "num_class_splitting_relocation_classes";
constexpr const char* METRIC_RELOCATED_STATIC_METHODS =
    "num_class_splitting_relocated_static_methods";
constexpr const char* METRIC_RELOCATED_NON_STATIC_DIRECT_METHODS =
    "num_class_splitting_relocated_non_static_direct_methods";
constexpr const char* METRIC_RELOCATED_NON_TRUE_VIRTUAL_METHODS =
    "num_class_splitting_relocated_non_true_virtual_methods";
constexpr const char* METRIC_RELOCATED_TRUE_VIRTUAL_METHODS =
    "num_class_splitting_relocated_true_virtual_methods";
constexpr const char* METRIC_NON_RELOCATED_METHODS =
    "num_class_splitting_non_relocated_methods";
constexpr const char* METRIC_POPULAR_METHODS =
    "num_class_splitting_popular_methods";
constexpr const char* METRIC_SOURCE_BLOCKS_POSITIVE_VALS =
    "num_class_splitting_source_block_positive_vals";
constexpr const char* METRIC_RELOCATED_METHODS =
    "num_class_splitting_relocated_methods";
constexpr const char* METRIC_TRAMPOLINES = "num_class_splitting_trampolines";

constexpr const char* RELOCATED_SUFFIX = "$relocated;";

struct ClassSplittingStats {
  size_t relocation_classes{0};
  size_t relocated_static_methods{0};
  size_t relocated_non_static_direct_methods{0};
  size_t relocated_non_true_virtual_methods{0};
  size_t relocated_true_virtual_methods{0};
  size_t non_relocated_methods{0};
  size_t popular_methods{0};
  size_t source_block_positive_vals{0};
};

class ClassSplittingImpl {
 public:
  ClassSplittingImpl(const ClassSplittingConfig& config,
                     PassManager& mgr,
                     const method_profiles::MethodProfiles& method_profiles)
      : m_config(config), m_mgr(mgr), m_method_profiles(method_profiles) {}

  void configure(const Scope& scope) {
    always_assert(m_method_profiles.has_stats());

    for (auto& p : m_method_profiles.all_interactions()) {
      auto& method_stats = p.second;
      walk::methods(scope, [&](DexMethod* method) {
        auto it = method_stats.find(method);
        if (it == method_stats.end()) {
          return;
        }
        if (it->second.appear_percent >=
            m_config.method_profiles_appear_percent_threshold) {
          m_sufficiently_popular_methods.insert(method);
        } else {
          m_insufficiently_popular_methods.insert(method);
        }
      });
    }

    if (m_config.relocate_non_true_virtual_methods) {
      m_non_true_virtual_methods = method_override_graph::get_non_true_virtuals(
          *method_override_graph::build_graph(scope), scope);
    }
  };

  DexClass* create_target_class(const std::string& target_type_name) {
    DexType* target_type = DexType::make_type(target_type_name.c_str());
    ++m_stats.relocation_classes;
    ClassCreator cc(target_type);
    cc.set_access(ACC_PUBLIC | ACC_FINAL);
    cc.set_super(type::java_lang_Object());
    auto target_cls = cc.create();
    target_cls->rstate.set_generated();
    target_cls->set_deobfuscated_name(target_type_name);
    return target_cls;
  }

  DexMethod* create_trampoline_method(DexMethod* method,
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

  bool has_source_block_positive_val(DexMethod* method) {
    for (auto& mie : *method->get_code()) {
      if (mie.type == MFLOW_SOURCE_BLOCK &&
          source_blocks::has_source_block_positive_val(mie.src_block.get())) {
        return true;
      }
    }
    return false;
  }

  void prepare(const DexClass* cls,
               std::vector<DexMethodRef*>* mrefs,
               std::vector<DexType*>* trefs,
               bool should_not_relocate_methods_of_class) {
    // Bail out if we just cannot or should not relocate methods of this class.
    if (!can_relocate(cls) || should_not_relocate_methods_of_class) {
      return;
    }
    auto cls_has_problematic_clinit = method::clinit_may_have_side_effects(cls);

    SplitClass& sc = m_split_classes[cls];
    always_assert(sc.relocatable_methods.empty());
    auto process_method = [&](DexMethod* method) {
      if (!method->get_code()) {
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
          ss << "Lredex/$Relocated"
             << std::to_string(m_next_target_class_index++) << "ApiLevel"
             << std::to_string(api_level) << ";";
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
          target_cls = create_target_class(
              source_name.substr(0, source_name.size() - 1) + RELOCATED_SUFFIX);
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
    };
    auto& dmethods = cls->get_dmethods();
    std::for_each(dmethods.begin(), dmethods.end(), process_method);
    auto& vmethods = cls->get_vmethods();
    std::for_each(vmethods.begin(), vmethods.end(), process_method);
  }

  DexClasses additional_classes(const DexClassesVector& outdex,
                                const DexClasses& classes) {
    // Here, we are going to do the final determination of what to relocate ---
    // After checking if things still look as they did before, and no other
    // interdex pass or feature tinkered with the relocatability...
    // The actual relocation will happen in cleanup, so that we don't interfere
    // with earlier InterDex cleanups that still expect the code to be in their
    // original places.

    DexClasses target_classes;
    std::unordered_set<const DexClass*> target_classes_set;
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
      auto cls_has_problematic_clinit =
          method::clinit_may_have_side_effects(cls);
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
        } else if (m_non_true_virtual_methods.count(method)) {
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

  void materialize_trampoline_code(DexMethod* source, DexMethod* target) {
    // "source" is the original method, still in its original place.
    // "target" is the new trampoline target method, somewhere far away
    target->set_code(std::make_unique<IRCode>(*source->get_code()));
    source->set_code(std::make_unique<IRCode>());
    auto code = source->get_code();
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
        load_param_insn->set_dest(code->allocate_wide_temp());
      } else {
        load_param_insn =
            new IRInstruction(type::is_object(t) ? IOPCODE_LOAD_PARAM_OBJECT
                                                 : IOPCODE_LOAD_PARAM);
        load_param_insn->set_dest(code->allocate_temp());
      }
      code->push_back(load_param_insn);
      invoke_insn->set_src(i, load_param_insn->dest());
    }
    code->push_back(invoke_insn);
    IRInstruction* return_insn;
    if (proto->get_rtype() != type::_void()) {
      auto t = proto->get_rtype();
      IRInstruction* move_result_insn;
      if (type::is_wide_type(t)) {
        move_result_insn = new IRInstruction(OPCODE_MOVE_RESULT_WIDE);
        move_result_insn->set_dest(code->allocate_wide_temp());
        return_insn = new IRInstruction(OPCODE_RETURN_WIDE);
      } else {
        move_result_insn =
            new IRInstruction(type::is_object(t) ? OPCODE_MOVE_RESULT_OBJECT
                                                 : OPCODE_MOVE_RESULT);
        move_result_insn->set_dest(code->allocate_temp());
        return_insn = new IRInstruction(
            type::is_object(t) ? OPCODE_RETURN_OBJECT : OPCODE_RETURN);
      }
      code->push_back(move_result_insn);
      return_insn->set_src(0, move_result_insn->dest());
    } else {
      return_insn = new IRInstruction(OPCODE_RETURN_VOID);
    }
    code->push_back(return_insn);
    TRACE(CS, 5, "[class splitting] New body for {%s}: \n%s", SHOW(source),
          SHOW(code));
    change_visibility(target);
  }

  void cleanup(const Scope& final_scope) {
    // Here we do the actual relocation.

    // Part 1: Upgrade non-static invokes to static invokes
    std::unordered_set<DexMethod*> methods_to_staticize;
    for (auto& p : m_methods_to_relocate) {
      DexMethod* method = p.first;
      if (!is_static(method)) {
        methods_to_staticize.insert(method);
      }
    }

    // We now rewrite all invoke-instructions as needed to reflect the fact that
    // we made some methods static as part of the relocation effort.
    std::unordered_map<IROpcode, std::atomic<size_t>, boost::hash<IROpcode>>
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
            auto resolved_method = resolve_method(
                insn->get_method(), opcode_to_search(insn), method);
            if (resolved_method &&
                methods_to_staticize.count(resolved_method)) {
              insn->set_opcode(OPCODE_INVOKE_STATIC);
              insn->set_method(resolved_method);
              rewritten_invokes.at(op)++;
            }
            break;
          }
          case OPCODE_INVOKE_INTERFACE:
          case OPCODE_INVOKE_STATIC: {
            auto resolved_method = resolve_method(
                insn->get_method(), opcode_to_search(insn), method);
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
    for (auto& p : rewritten_invokes) {
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
      relocate_method(method, target_cls->get_type());
      change_visibility(method);
    }
    TRACE(CS, 2, "[class splitting] Made {%zu} methods static.",
          methods_to_staticize.size());

    // Part 3: Materialize trampolines
    for (auto& p : m_methods_to_trampoline) {
      materialize_trampoline_code(p.first, p.second);
    }

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

 private:
  std::unordered_set<DexMethod*> m_sufficiently_popular_methods;
  // Methods that appear in the profiles and whose frequency does not exceed
  // the threashold.
  std::unordered_set<DexMethod*> m_insufficiently_popular_methods;

  struct RelocatableMethodInfo {
    DexClass* target_cls;
    DexMethod* trampoline_target_method;
    int32_t api_level;
  };

  struct SplitClass {
    std::unordered_map<DexMethod*, RelocatableMethodInfo> relocatable_methods;
  };

  struct TargetClassInfo {
    DexClass* target_cls{nullptr};
    const DexClass* last_source_cls{nullptr};
    size_t size{0}; // number of methods
  };

  std::unordered_map<int32_t, TargetClassInfo> m_target_classes_by_api_level;
  size_t m_next_target_class_index{0};
  std::unordered_map<DexType*, DexClass*> m_target_classes_by_source_classes;
  std::unordered_map<const DexClass*, SplitClass> m_split_classes;
  std::vector<std::pair<DexMethod*, DexClass*>> m_methods_to_relocate;
  std::vector<std::pair<DexMethod*, DexMethod*>> m_methods_to_trampoline;
  ClassSplittingStats m_stats;
  std::unordered_set<DexMethod*> m_non_true_virtual_methods;

  bool matches(const std::string& name, const std::string& v) {
    return name.find(v) != std::string::npos;
  }
  bool can_relocate(const DexClass* cls) {
    return !cls->is_external() && !cls->rstate.is_generated() &&
           std::find_if(m_config.blocklist_types.begin(),
                        m_config.blocklist_types.end(),
                        [&](const std::string& v) {
                          return matches(cls->c_str(), v);
                        }) == m_config.blocklist_types.end();
  }

  bool can_relocate(bool cls_has_problematic_clinit,
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
    if (!get_visibility_changes(m).empty()) {
      if (log) {
        m_mgr.incr_metric(
            "num_class_splitting_limitation_cannot_change_visibility", 1);
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
          m_mgr.incr_metric(
              "num_class_splitting_limitation_static_method_is_clinit", 1);
        }
        // TODO: Could be done with trampolines if we remove "final" flag from
        // fields, and carefully deal with super-init calls.
        return false;
      }
    } else if (m_non_true_virtual_methods.count(const_cast<DexMethod*>(m))) {
      if (!m_config.relocate_non_true_virtual_methods) {
        return false;
      }
    } else {
      if (!m_config.relocate_true_virtual_methods) {
        return false;
      }
      *requires_trampoline = true;
    }
    if (*requires_trampoline && m->get_code()->sum_opcode_sizes() <
                                    m_config.trampoline_size_threshold) {
      if (log) {
        m_mgr.incr_metric(
            "num_class_splitting_trampoline_size_threshold_not_met", 1);
      }
      return false;
    }
    return true;
  };

  ClassSplittingConfig m_config;
  PassManager& m_mgr;
  const method_profiles::MethodProfiles& m_method_profiles;
};

void update_coldstart_classes_order(
    ConfigFiles& conf,
    PassManager& mgr,
    const std::unordered_set<DexType*>& coldstart_types,
    const std::vector<std::string>& previously_relocated_types,
    bool log = true) {
  const auto& coldstart_classes = conf.get_coldstart_classes();

  std::unordered_map<std::string, std::string> replacement;
  for (const auto& str : previously_relocated_types) {
    auto initial_type = str.substr(0, str.size() - 11) + ";";

    auto type = DexType::get_type(initial_type.c_str());
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

} // namespace

void ClassSplittingPass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  TRACE(CS, 1, "[class splitting] Enabled: %d", m_config.enabled);
  if (!m_config.enabled) {
    return;
  }

  const auto& method_profiles = conf.get_method_profiles();
  if (!method_profiles.has_stats()) {
    TRACE(CS, 1,
          "[class splitting] Disabled since we don't have method profiles");
    return;
  }

  // We are going to simulate how the InterDex pass would invoke our plug-in in
  // a way that can run before the actual InterDex pass. Then, the actual
  // InterDex pass run can reshuffle the split-off classes across dexes
  // properly, accounting for all the changes to refs from the beginning.
  ClassSplittingImpl class_splitting_impl(m_config, mgr,
                                          conf.get_method_profiles());
  auto scope = build_class_scope(stores);
  class_splitting_impl.configure(scope);
  std::unordered_set<DexType*> coldstart_types;
  std::vector<std::string> previously_relocated_types;
  for (const auto& str : conf.get_coldstart_classes()) {
    DexType* type = DexType::get_type(str.c_str());
    if (type) {
      coldstart_types.insert(type);
    } else if (boost::algorithm::ends_with(str, RELOCATED_SUFFIX)) {
      previously_relocated_types.emplace_back(str);
    }
  }

  // Since classes that we previously split and ONLY the relocated part appears
  // in coldstart types won't be actually split this time, we also need to
  // update the initial class ordering to reflect that.
  update_coldstart_classes_order(conf, mgr, coldstart_types,
                                 previously_relocated_types);

  // In a clandestine way, we create instances of all InterDex plugins on the
  // side in order to check if we should skip a class for some obscure reason.
  interdex::InterDexRegistry* registry =
      static_cast<interdex::InterDexRegistry*>(
          PluginRegistry::get().pass_registry(interdex::INTERDEX_PASS_NAME));
  auto plugins = registry->create_plugins();

  TRACE(CS, 2,
        "[class splitting] Operating on %zu cold-start types and %zu plugins",
        coldstart_types.size(), plugins.size());

  auto should_skip = [&](DexClass* cls) {
    for (auto& plugin : plugins) {
      if (plugin->should_skip_class(cls)) {
        return true;
      }
    }
    return false;
  };
  auto should_not_relocate_methods_of_class = [&](DexClass* cls) {
    for (auto& plugin : plugins) {
      if (plugin->should_not_relocate_methods_of_class(cls)) {
        return true;
      }
    }
    return false;
  };

  // We are only going to perform class-splitting in the first store, as that's
  // where all the perf-sensitive classes are.
  auto& store = stores.at(0);
  auto& dexen = store.get_dexen();
  DexClasses classes;
  // We skip the first dex, as that's the primary dex, and we won't split
  // classes in there anyway.
  for (size_t dex_nr = 1; dex_nr < dexen.size(); dex_nr++) {
    auto& dex = dexen.at(dex_nr);
    for (auto cls : dex) {
      if (!coldstart_types.count(cls->get_type()) &&
          !cls->rstate.has_interdex_subgroup()) {
        continue;
      }
      if (should_skip(cls)) {
        continue;
      }
      classes.push_back(cls);
      class_splitting_impl.prepare(cls, nullptr /* mrefs */,
                                   nullptr /* trefs */,
                                   should_not_relocate_methods_of_class(cls));
    }
  }
  auto classes_to_add = class_splitting_impl.additional_classes(dexen, classes);
  dexen.push_back(classes_to_add);
  TRACE(CS, 1, "[class splitting] Added %zu classes", classes_to_add.size());
  auto final_scope = build_class_scope(stores);
  class_splitting_impl.cleanup(final_scope);
}

static ClassSplittingPass s_pass;
