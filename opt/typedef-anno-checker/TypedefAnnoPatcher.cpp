/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoPatcher.h"

#include "AnnoUtils.h"
#include "ClassUtil.h"
#include "KotlinNullCheckMethods.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

constexpr const char* INT_REF_CLS = "Lkotlin/jvm/internal/Ref$IntRef;";
constexpr const char* INT_REF_FIELD =
    "Lkotlin/jvm/internal/Ref$IntRef;.element:I";
constexpr const char* OBJ_REF_CLS = "Lkotlin/jvm/internal/Ref$ObjectRef;";
constexpr const char* OBJ_REF_FIELD =
    "Lkotlin/jvm/internal/Ref$ObjectRef;.element:Ljava/lang/Object;";

namespace typedef_anno {
bool is_int(const type_inference::TypeEnvironment& env, reg_t reg) {
  return env.get_dex_type(reg) != boost::none &&
         type::is_int(*env.get_dex_type(reg));
}

// if there's no dex type, the value is null, and the checker does not enforce
// nullability
bool is_string(const type_inference::TypeEnvironment& env, reg_t reg) {
  return env.get_dex_type(reg) != boost::none
             ? *env.get_dex_type(reg) == type::java_lang_String()
             : true;
}

bool is_not_str_nor_int(const type_inference::TypeEnvironment& env, reg_t reg) {
  return !is_string(env, reg) && !is_int(env, reg);
}

bool is_int_or_obj_ref(const type_inference::TypeEnvironment& env, reg_t reg) {
  return env.get_dex_type(reg) != boost::none
             ? (*env.get_dex_type(reg) == DexType::make_type(INT_REF_CLS) ||
                *env.get_dex_type(reg) == DexType::make_type(OBJ_REF_CLS))
             : true;
}
} // namespace typedef_anno

namespace {

bool has_typedef_annos(ParamAnnotations* param_annos,
                       const std::unordered_set<DexType*>& typedef_annos) {
  if (!param_annos) {
    return false;
  }
  for (auto& anno : *param_annos) {
    auto& anno_set = anno.second;
    auto typedef_anno = type_inference::get_typedef_annotation(
        anno_set->get_annotations(), typedef_annos);
    if (typedef_anno != boost::none) {
      return true;
    }
  }
  return false;
}

DexMethod* resolve_method(DexMethod* caller, IRInstruction* insn) {
  auto def_method =
      resolve_method(insn->get_method(), opcode_to_search(insn), caller);
  if (def_method == nullptr && insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
    def_method =
        resolve_method(insn->get_method(), MethodSearch::InterfaceVirtual);
  }
  return def_method;
}

/**
 * A class that resembles a fun interface class like the ones in P1720288509 and
 * P1720292631.
 */
bool is_fun_interface_class(const DexClass* cls) {
  if (!klass::maybe_anonymous_class(cls)) {
    return false;
  }
  if (cls->get_super_class() == type::java_lang_Object() &&
      cls->get_interfaces()->size() != 1) {
    return false;
  }
  if (!cls->get_sfields().empty()) {
    return false;
  }
  for (const auto* f : cls->get_ifields()) {
    if (!is_synthetic(f)) {
      return false;
    }
  }
  if (cls->get_ctors().size() != 1) {
    return false;
  }
  const auto& vmethods = cls->get_vmethods();
  if (vmethods.empty()) {
    return false;
  }
  return true;
}

/**
 * Kotlinc style synthesized lambda class (not D8 desugared style). An example
 * is shared in P1690836921.
 */
bool is_synthesized_lambda_class(const DexClass* cls) {
  if (!klass::maybe_anonymous_class(cls)) {
    return false;
  }
  if (cls->get_super_class() !=
      DexType::make_type("Lkotlin/jvm/internal/Lambda;")) {
    return false;
  }
  if (cls->get_interfaces()->size() != 1) {
    return false;
  }
  const auto* intf = cls->get_interfaces()->at(0);
  if (!boost::starts_with(intf->get_name()->str(),
                          "Lkotlin/jvm/functions/Function")) {
    return false;
  }
  if (!cls->get_sfields().empty()) {
    return false;
  }
  for (const auto* f : cls->get_ifields()) {
    if (!is_synthetic(f)) {
      return false;
    }
  }
  if (cls->get_ctors().size() != 1) {
    return false;
  }
  const auto vmethods = cls->get_vmethods();
  if (vmethods.empty()) {
    return false;
  }
  for (const auto* m : vmethods) {
    const auto name = m->get_simple_deobfuscated_name();
    if (name != "invoke") {
      return false;
    }
  }
  return true;
}

DexMethodRef* get_enclosing_method(DexClass* cls) {
  auto anno_set = cls->get_anno_set();
  if (!anno_set) {
    return nullptr;
  }
  DexAnnotation* anno = get_annotation(
      cls, DexType::make_type("Ldalvik/annotation/EnclosingMethod;"));
  if (anno) {
    auto& value = anno->anno_elems().begin()->encoded_value;
    if (value->evtype() == DexEncodedValueTypes::DEVT_METHOD) {
      auto method_value = static_cast<DexEncodedValueMethod*>(value.get());
      auto method_name = method_value->show_deobfuscated();
      return DexMethod::get_method(method_name);
    }
  }
  return nullptr;
}

// make the methods and fields temporarily synthetic to add annotations
template <typename DexMember>
bool add_annotations(DexMember* member,
                     DexAnnotationSet* anno_set,
                     PatcherStats& class_stats) {
  if (member && member->is_def()) {
    auto def_member = member->as_def();
    auto existing_annos = def_member->get_anno_set();
    if (existing_annos) {
      size_t anno_size = existing_annos->get_annotations().size();
      existing_annos->combine_with(*anno_set);
      if (existing_annos->get_annotations().size() != anno_size) {
        class_stats.num_patched_fields_and_methods += 1;
      }
    } else {
      DexAccessFlags access = def_member->get_access();
      def_member->set_access(ACC_SYNTHETIC);
      auto res = def_member->attach_annotation_set(
          std::make_unique<DexAnnotationSet>(*anno_set));
      always_assert(res);
      def_member->set_access(access);
      class_stats.num_patched_fields_and_methods += 1;
    }
    return true;
  }
  return false;
}

void add_param_annotations(DexMethod* m,
                           DexAnnotationSet* anno_set,
                           int param,
                           PatcherStats& class_stats) {
  if (m->get_param_anno()) {
    if (m->get_param_anno()->count(param) == 1) {
      std::unique_ptr<DexAnnotationSet>& param_anno_set =
          m->get_param_anno()->at(param);
      if (param_anno_set != nullptr) {
        size_t anno_size = param_anno_set->get_annotations().size();
        param_anno_set->combine_with(*anno_set);
        if (param_anno_set->get_annotations().size() != anno_size) {
          class_stats.num_patched_parameters += 1;
        }
        return;
      }
    }
  }
  DexAccessFlags access = m->get_access();
  m->set_access(ACC_SYNTHETIC);
  m->attach_param_annotation_set(param,
                                 std::make_unique<DexAnnotationSet>(*anno_set));
  m->set_access(access);
}

// Run usedefs to see if the source of the annotated value is a parameter
// If it is, return the index the parameter. If not, return -1
int find_and_patch_parameter(
    DexMethod* m,
    IRInstruction* insn,
    src_index_t arg_index,
    DexAnnotationSet* anno_set,
    live_range::UseDefChains* ud_chains,
    PatcherStats& class_stats,
    std::vector<std::pair<src_index_t, DexAnnotationSet&>>*
        missing_param_annos = nullptr) {
  // Patch missing parameter annotations from accessed fields
  live_range::Use use_of_id{insn, arg_index};
  auto udchains_it = ud_chains->find(use_of_id);
  auto defs_set = udchains_it->second;

  for (auto* def : defs_set) {
    if (!opcode::is_a_load_param(def->opcode())) {
      continue;
    }
    auto param_index = 0;
    for (auto& mie :
         InstructionIterable(m->get_code()->cfg().get_param_instructions())) {
      if (mie.insn == def) {
        if (!is_static(m)) {
          param_index -= 1;
        }
        if (!missing_param_annos) {
          add_param_annotations(m, anno_set, param_index, class_stats);
        }
        return param_index;
      }
      param_index += 1;
    }
  }
  return -1;
}

void patch_param_from_method_invoke(
    TypeEnvironments& envs,
    type_inference::TypeInference& inference,
    DexMethod* caller,
    IRInstruction* insn,
    live_range::UseDefChains* ud_chains,
    PatcherStats& class_stats,
    std::vector<std::pair<src_index_t, DexAnnotationSet&>>*
        missing_param_annos = nullptr,
    bool patch_accessor = true) {
  always_assert(opcode::is_an_invoke(insn->opcode()));
  auto* def_method = resolve_method(caller, insn);
  if (!def_method ||
      (!def_method->get_param_anno() && !def_method->get_anno_set())) {
    // callee cannot be resolved, has no param annotation, or has no return
    // annotation
    return;
  }

  auto& env = envs.find(insn)->second;
  if (def_method->get_param_anno()) {
    for (auto const& param_anno : *def_method->get_param_anno()) {
      auto annotation = type_inference::get_typedef_annotation(
          param_anno.second->get_annotations(), inference.get_annotations());
      if (!annotation) {
        continue;
      }
      int arg_index = insn->opcode() == OPCODE_INVOKE_STATIC
                          ? param_anno.first
                          : param_anno.first + 1;
      reg_t arg_reg = insn->src(arg_index);
      auto anno_type = env.get_annotation(arg_reg);
      if (anno_type && anno_type == annotation) {
        // Safe assignment. Nothing to do.
        continue;
      }
      DexAnnotationSet anno_set = DexAnnotationSet();
      anno_set.add_annotation(std::make_unique<DexAnnotation>(
          DexType::make_type(annotation.get()->get_name()), DAV_RUNTIME));
      auto param_index = find_and_patch_parameter(
          caller, insn, arg_index, &anno_set, ud_chains, class_stats);
      DexAnnotationSet& param_anno_set = *param_anno.second;
      if (missing_param_annos && param_index != -1) {
        missing_param_annos->push_back({param_index, param_anno_set});
      }
      TRACE(TAC, 2, "Missing param annotation %s in %s",
            SHOW(param_anno.second), SHOW(caller));
    }
  }
}

void patch_setter_method(type_inference::TypeInference& inference,
                         DexMethod* caller,
                         IRInstruction* insn,
                         live_range::UseDefChains* ud_chains,
                         PatcherStats& class_stats,
                         std::vector<std::pair<src_index_t, DexAnnotationSet&>>*
                             missing_param_annos = nullptr) {
  always_assert(opcode::is_an_iput(insn->opcode()) ||
                opcode::is_an_sput(insn->opcode()));
  auto field_ref = insn->get_field();
  auto field_anno = type_inference::get_typedef_anno_from_member(
      field_ref, inference.get_annotations());

  if (field_anno != boost::none) {
    auto param_index = find_and_patch_parameter(
        caller, insn, 0, field_ref->as_def()->get_anno_set(), ud_chains,
        class_stats);
    if (missing_param_annos && param_index != -1) {
      missing_param_annos->push_back(
          {param_index, *field_ref->as_def()->get_anno_set()});
    }
  }
}

} // namespace

// https://kotlinlang.org/docs/fun-interfaces.html#sam-conversions
// sam conversions appear in Kotlin and provide a more concise way to override
// methods. This method handles sam conversiona and all synthetic methods that
// override methods with return or parameter annotations
bool TypedefAnnoPatcher::patch_synth_methods_overriding_annotated_methods(
    DexMethod* m, PatcherStats& class_stats) {
  DexClass* cls = type_class(m->get_class());
  if (!klass::maybe_anonymous_class(cls)) {
    return false;
  }

  auto callees = mog::get_overridden_methods(m_method_override_graph, m,
                                             true /*include_interfaces*/);
  for (auto callee : callees) {
    auto return_anno =
        type_inference::get_typedef_anno_from_member(callee, m_typedef_annos);

    if (return_anno != boost::none) {
      DexAnnotationSet anno_set = DexAnnotationSet();
      anno_set.add_annotation(std::make_unique<DexAnnotation>(
          DexType::make_type(return_anno.get()->get_name()), DAV_RUNTIME));
      add_annotations(m, &anno_set, class_stats);
    }

    if (callee->get_param_anno() == nullptr) {
      continue;
    }
    for (auto const& param_anno : *callee->get_param_anno()) {
      auto annotation = type_inference::get_typedef_annotation(
          param_anno.second->get_annotations(), m_typedef_annos);
      if (annotation == boost::none) {
        continue;
      }

      DexAnnotationSet anno_set = DexAnnotationSet();
      anno_set.add_annotation(std::make_unique<DexAnnotation>(
          DexType::make_type(annotation.get()->get_name()), DAV_RUNTIME));
      add_param_annotations(m, &anno_set, param_anno.first, class_stats);
    }
  }
  return false;
}

void TypedefAnnoPatcher::run(const Scope& scope) {
  m_patcher_stats =
      walk::parallel::classes<PatcherStats>(scope, [this](DexClass* cls) {
        auto class_stats = PatcherStats();
        if (is_synthesized_lambda_class(cls) || is_fun_interface_class(cls)) {
          std::vector<const DexField*> patched_fields;
          for (auto m : cls->get_all_methods()) {
            patch_lambdas(m, &patched_fields, class_stats);
          }
          if (!patched_fields.empty()) {
            auto cls_name = cls->get_deobfuscated_name_or_empty_copy();
            auto common_class_name_end = cls_name.find('$') + 1;
            if (cls_name[common_class_name_end] < '0' ||
                cls_name[common_class_name_end] > '9') {
              common_class_name_end =
                  cls_name.find('$', cls_name.find('$') + 1);
            }
            auto class_prefix = cls_name.substr(0, common_class_name_end);
            m_lambda_anno_map.emplace(class_prefix, patched_fields);
          }
        }
        for (auto m : cls->get_all_methods()) {
          patch_parameters_and_returns(m, class_stats);
          patch_synth_methods_overriding_annotated_methods(m, class_stats);
          if (is_constructor(m) &&
              has_typedef_annos(m->get_param_anno(), m_typedef_annos)) {
            patch_synth_cls_fields_from_ctor_param(m, class_stats);
          }
        }
        return class_stats;
      });

  m_chained_patcher_stats =
      walk::parallel::classes<PatcherStats>(scope, [&](DexClass* cls) {
        auto class_stats = PatcherStats();
        if (klass::maybe_anonymous_class(cls) && get_enclosing_method(cls)) {
          patch_enclosed_method(cls, class_stats);
          patch_ctor_params_from_synth_cls_fields(cls, class_stats);
        }
        populate_chained_getters(cls);
        return class_stats;
      });

  patch_chained_getters(m_chained_getter_patcher_stats);
}

void TypedefAnnoPatcher::populate_chained_getters(DexClass* cls) {
  auto class_name = cls->get_deobfuscated_name_or_empty_copy();
  auto class_name_prefix = class_name.substr(0, class_name.find('$'));
  if (m_patched_returns.count(class_name_prefix)) {
    m_chained_getters.insert(cls);
  }
}

void TypedefAnnoPatcher::patch_chained_getters(PatcherStats& class_stats) {
  std::vector<DexClass*> sorted_candidates;
  for (auto* cls : m_chained_getters) {
    sorted_candidates.push_back(cls);
  }
  std::sort(sorted_candidates.begin(), sorted_candidates.end(),
            compare_dexclasses);
  for (auto* cls : sorted_candidates) {
    for (auto m : cls->get_all_methods()) {
      patch_parameters_and_returns(m, class_stats);
    }
  }
}

void TypedefAnnoPatcher::patch_ctor_params_from_synth_cls_fields(
    DexClass* cls, PatcherStats& class_stats) {
  bool has_annotated_fields = false;
  for (auto field : cls->get_ifields()) {
    DexAnnotationSet* anno_set = field->get_anno_set();
    if (anno_set &&
        type_inference::get_typedef_annotation(
            anno_set->get_annotations(), m_typedef_annos) != boost::none) {
      has_annotated_fields = true;
    }
  }
  // if no fields have typedef annotations, there is no need to patch the
  // constructor
  if (!has_annotated_fields) {
    return;
  }

  for (auto ctor : cls->get_ctors()) {
    IRCode* ctor_code = ctor->get_code();
    auto& ctor_cfg = ctor_code->cfg();
    type_inference::TypeInference ctor_inference(
        ctor_cfg, /*skip_check_cast_upcasting*/ false, m_typedef_annos,
        &m_method_override_graph);
    ctor_inference.run(ctor);
    TypeEnvironments& ctor_envs = ctor_inference.get_type_environments();

    live_range::MoveAwareChains ctor_chains(ctor_cfg);
    live_range::DefUseChains ctor_du_chains = ctor_chains.get_def_use_chains();
    size_t param_idx = 0;
    for (cfg::Block* b : ctor_cfg.blocks()) {
      for (auto& mie : InstructionIterable(b)) {
        auto* insn = mie.insn;
        if (!opcode::is_a_load_param(insn->opcode())) {
          continue;
        }
        param_idx++;
        auto param_anno = ctor_envs.at(insn).get_annotation(insn->dest());
        if (param_anno != boost::none) {
          continue;
        }
        auto& env = ctor_envs.at(insn);
        if (typedef_anno::is_not_str_nor_int(env, insn->dest()) &&
            !typedef_anno::is_int_or_obj_ref(env, insn->dest())) {
          continue;
        }
        auto udchains_it = ctor_du_chains.find(insn);
        auto uses_set = udchains_it->second;
        for (live_range::Use use : uses_set) {
          IRInstruction* use_insn = use.insn;
          if (!opcode::is_an_iput(use_insn->opcode())) {
            continue;
          }
          auto field = use_insn->get_field()->as_def();
          if (!field) {
            continue;
          }
          auto field_anno = type_inference::get_typedef_anno_from_member(
              use_insn->get_field(), ctor_inference.get_annotations());
          if (field_anno == boost::none) {
            continue;
          }
          DexAnnotationSet anno_set = DexAnnotationSet();
          anno_set.add_annotation(std::make_unique<DexAnnotation>(
              DexType::make_type(field_anno.get()->get_name()), DAV_RUNTIME));
          add_param_annotations(ctor, &anno_set, param_idx - 2, class_stats);
        }
      }
    }
  }
}

void patch_synthetic_field_from_local_var_lambda(
    const live_range::UseDefChains& ud_chains,
    IRInstruction* insn,
    const src_index_t src,
    DexAnnotationSet* anno_set,
    std::vector<const DexField*>* patched_fields,
    PatcherStats& class_stats) {
  live_range::Use use_of_id{insn, src};
  auto udchains_it = ud_chains.find(use_of_id);
  auto defs_set = udchains_it->second;
  for (IRInstruction* def : defs_set) {
    DexField* field;
    if (def->opcode() == OPCODE_CHECK_CAST) {
      live_range::Use cc_use_of_id{def, (src_index_t)(0)};
      auto cc_udchains_it = ud_chains.find(cc_use_of_id);
      auto cc_defs_set = cc_udchains_it->second;
      for (IRInstruction* cc_def : cc_defs_set) {
        if (!opcode::is_an_iget(cc_def->opcode())) {
          continue;
        }
        field = cc_def->get_field()->as_def();
      }
    } else if (opcode::is_an_iget(def->opcode())) {
      field = def->get_field()->as_def();
    } else {
      continue;
    }
    if (!field) {
      continue;
    }

    if (field->get_deobfuscated_name_or_empty() == INT_REF_FIELD ||
        field->get_deobfuscated_name_or_empty() == OBJ_REF_FIELD) {
      live_range::Use ref_use_of_id{def, 0};
      auto ref_udchains_it = ud_chains.find(ref_use_of_id);
      auto ref_defs_set = ref_udchains_it->second;
      for (IRInstruction* ref_def : ref_defs_set) {
        if (!opcode::is_an_iget(ref_def->opcode())) {
          continue;
        }
        auto original_field = ref_def->get_field()->as_def();
        if (!original_field) {
          continue;
        }
        patched_fields->push_back(original_field);
        add_annotations(original_field, anno_set, class_stats);
      }
    } else {
      patched_fields->push_back(field);
      add_annotations(field, anno_set, class_stats);
    }
  }
}

// Given a method, named 'callee', inside a lambda and the UseDefChains and
// TypeInference of the synthetic caller method, check if the callee has
// annotated parameters. If it does, finds the synthetic field representing the
// local variable that was passed into the callee and annotate it.
void annotate_local_var_field_from_callee(
    const DexMethod* callee,
    IRInstruction* insn,
    const live_range::UseDefChains& ud_chains,
    const type_inference::TypeInference& inference,
    std::vector<const DexField*>* patched_fields,
    PatcherStats& class_stats) {
  if (!callee) {
    return;
  }
  if (!callee->get_param_anno()) {
    return;
  }
  for (auto const& param_anno : *callee->get_param_anno()) {
    auto annotation = type_inference::get_typedef_annotation(
        param_anno.second->get_annotations(), inference.get_annotations());
    if (annotation != boost::none) {
      DexAnnotationSet anno_set = DexAnnotationSet();
      anno_set.add_annotation(std::make_unique<DexAnnotation>(
          DexType::make_type(annotation.get()->get_name()), DAV_RUNTIME));
      patch_synthetic_field_from_local_var_lambda(
          ud_chains, insn, param_anno.first + 1, &anno_set, patched_fields,
          class_stats);
    }
  }
}

// Check all lambdas and function interfaces for any fields that need to be
// patched
void TypedefAnnoPatcher::patch_lambdas(
    DexMethod* method,
    std::vector<const DexField*>* patched_fields,
    PatcherStats& class_stats) {
  IRCode* code = method->get_code();
  if (!code) {
    return;
  }

  always_assert_log(code->editable_cfg_built(), "%s has no cfg built",
                    SHOW(method));
  auto& cfg = code->cfg();

  type_inference::TypeInference inference(cfg, false, m_typedef_annos,
                                          &m_method_override_graph);
  inference.run(method);
  live_range::MoveAwareChains chains(cfg);
  live_range::UseDefChains ud_chains = chains.get_use_def_chains();
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      if (opcode::is_invoke_static(insn->opcode())) {
        DexMethod* static_method = insn->get_method()->as_def();
        if (!static_method) {
          continue;
        }
        // If the static method is synthetic, it might still expect annotated
        // parameters. Find any missing parameter annotations and add them to
        // the map of src index to annotation, but don't patch the static method
        // since a different visit of that method will patch it
        std::vector<std::pair<src_index_t, DexAnnotationSet&>>
            missing_param_annos;
        patch_parameters_and_returns(static_method, class_stats,
                                     &missing_param_annos);
        // Patch missing param annotations
        for (auto& pair : missing_param_annos) {
          patch_synthetic_field_from_local_var_lambda(
              ud_chains, insn, pair.first, &pair.second, patched_fields,
              class_stats);
        }
        // If the static method has parameter annotations, patch the synthetic
        // fields as expected
        if (static_method->get_param_anno()) {
          for (auto const& param_anno : *static_method->get_param_anno()) {
            auto annotation = type_inference::get_typedef_annotation(
                param_anno.second->get_annotations(), m_typedef_annos);
            if (annotation != boost::none) {
              patch_synthetic_field_from_local_var_lambda(
                  ud_chains, insn, param_anno.first, param_anno.second.get(),
                  patched_fields, class_stats);
            }
          }
        }
      } else if (opcode::is_invoke_interface(insn->opcode())) {
        auto* callee_def = resolve_method(method, insn);
        auto callees =
            mog::get_overriding_methods(m_method_override_graph, callee_def);
        for (auto callee : callees) {
          annotate_local_var_field_from_callee(
              callee, insn, ud_chains, inference, patched_fields, class_stats);
        }
      } else if (opcode::is_an_invoke(insn->opcode())) {
        auto* callee_def = resolve_method(method, insn);
        auto callees =
            mog::get_overriding_methods(m_method_override_graph, callee_def);
        callees.push_back(callee_def);
        for (auto callee : callees) {
          annotate_local_var_field_from_callee(
              callee, insn, ud_chains, inference, patched_fields, class_stats);
        }
      }
    }
  }
}

// Given a constructor of a synthetic class, check if it has typedef annotated
// parameters. If it does, find the field that the parameter got put into and
// annotate it.
void TypedefAnnoPatcher::patch_synth_cls_fields_from_ctor_param(
    DexMethod* ctor, PatcherStats& class_stats) {
  IRCode* code = ctor->get_code();
  if (!code) {
    return;
  }
  always_assert_log(code->editable_cfg_built(), "%s has no cfg built",
                    SHOW(ctor));
  auto& cfg = code->cfg();

  type_inference::TypeInference inference(cfg, false, m_typedef_annos,
                                          &m_method_override_graph);
  inference.run(ctor);
  TypeEnvironments& envs = inference.get_type_environments();
  auto class_name_dot = ctor->get_class()->get_name()->str() + ".";

  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      if (insn->opcode() != OPCODE_IPUT &&
          insn->opcode() != OPCODE_IPUT_OBJECT) {
        continue;
      }
      DexField* field = insn->get_field()->as_def();
      if (!field) {
        continue;
      }
      auto& env = envs.at(insn);
      if (!typedef_anno::is_int(env, insn->src(0)) &&
          !typedef_anno::is_string(env, insn->src(0))) {
        continue;
      }
      auto annotation = env.get_annotation(insn->src(0));
      if (annotation != boost::none) {
        DexAnnotationSet anno_set = DexAnnotationSet();
        anno_set.add_annotation(std::make_unique<DexAnnotation>(
            DexType::make_type(annotation.get()->get_name()), DAV_RUNTIME));
        add_annotations(field, &anno_set, class_stats);
        auto field_name = field->get_simple_deobfuscated_name();
        field_name.at(0) = std::toupper(field_name.at(0));
        const auto int_or_string =
            type::is_int(field->get_type())
                ? "I"
                : type::java_lang_String()->get_name()->str();
        // add annotations to the Kotlin getter and setter methods
        auto getter_method = DexMethod::get_method(
            class_name_dot + "get" + field_name + ":()" + int_or_string);
        if (getter_method) {
          patch_parameters_and_returns(getter_method->as_def(), class_stats);
        }
        auto setter_method = DexMethod::get_method(
            class_name_dot + "set" + field_name + ":(" + int_or_string + ")V");
        if (setter_method) {
          patch_parameters_and_returns(setter_method->as_def(), class_stats);
        }
      }
    }
  }
}

void TypedefAnnoPatcher::patch_enclosed_method(DexClass* cls,
                                               PatcherStats& class_stats) {
  auto cls_name = cls->get_deobfuscated_name_or_empty_copy();
  auto first_dollar = cls_name.find('$');
  always_assert_log(first_dollar != std::string::npos,
                    "The enclosed method class %s should have a $ in the name",
                    SHOW(cls));
  auto original_cls_name = cls_name.substr(0, first_dollar) + ";";
  DexClass* original_class =
      type_class(DexType::make_type(DexString::make_string(original_cls_name)));
  if (!original_class) {
    return;
  }

  auto second_dollar = cls_name.find('$', first_dollar + 1);
  auto fields = m_lambda_anno_map.get(cls_name.substr(0, second_dollar));
  if (!fields) {
    return;
  }

  for (auto field : *fields) {
    auto field_name = cls_name + "." + field->get_simple_deobfuscated_name() +
                      ":" + field->get_type()->str_copy();
    DexFieldRef* field_ref = DexField::get_field(field_name);
    if (field_ref && field->get_deobfuscated_name() != field_name) {
      DexAnnotationSet a_set = DexAnnotationSet();
      a_set.combine_with(*field->get_anno_set());
      DexField* dex_field = field_ref->as_def();
      if (dex_field) {
        add_annotations(dex_field, &a_set, class_stats);
      }
    }
  }
}

// This does 3 things if missing_param_annos is nullptr:
// 1. if a parameter is passed into an invoked method that expects an annotated
// argument, patch the parameter
// 2. if a parameter is passed into a field write and the field is annotated,
// patch the parameter
// 3. if all method returns are annotated as per TypeInference, patch the method
//
// if missing_param_annos is not nullptr, do not patch anything. Upon obtaining
// the parameter annotations, just add them to missing_param_annos
void TypedefAnnoPatcher::patch_parameters_and_returns(
    DexMethod* m,
    PatcherStats& class_stats,
    std::vector<std::pair<src_index_t, DexAnnotationSet&>>*
        missing_param_annos) {
  IRCode* code = m->get_code();
  if (!code) {
    return;
  }

  always_assert_log(code->editable_cfg_built(), "%s has no cfg built", SHOW(m));
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg, false, m_typedef_annos,
                                          &m_method_override_graph);
  inference.run(m);

  TypeEnvironments& envs = inference.get_type_environments();
  live_range::MoveAwareChains chains(m->get_code()->cfg());
  live_range::UseDefChains ud_chains = chains.get_use_def_chains();

  boost::optional<const DexType*> anno = boost::none;
  bool patch_return = missing_param_annos == nullptr;
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (opcode::is_an_invoke(opcode)) {
        patch_param_from_method_invoke(envs, inference, m, insn, &ud_chains,
                                       class_stats, missing_param_annos);
      } else if (opcode::is_an_iput(opcode) || opcode::is_an_sput(opcode)) {
        patch_setter_method(inference, m, insn, &ud_chains, class_stats,
                            missing_param_annos);
      } else if ((opcode == OPCODE_RETURN_OBJECT || opcode == OPCODE_RETURN) &&
                 patch_return) {
        auto return_anno = envs.at(insn).get_annotation(insn->src(0));
        if (return_anno == boost::none ||
            (anno != boost::none && return_anno != anno)) {
          patch_return = false;
        } else {
          anno = return_anno;
        }
      }
    }
  }

  if (patch_return && anno != boost::none) {
    DexAnnotationSet anno_set = DexAnnotationSet();
    anno_set.add_annotation(std::make_unique<DexAnnotation>(
        DexType::make_type(anno.get()->get_name()), DAV_RUNTIME));
    add_annotations(m, &anno_set, class_stats);
    auto class_name = type_class(m->get_class())->str();
    auto class_name_prefix = class_name.substr(0, class_name.size() - 1);
    if (!m_patched_returns.count(class_name_prefix)) {
      m_patched_returns.insert(class_name_prefix);
    }
  }
}
