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

constexpr const char* ACCESS_PREFIX = "access$";
constexpr const char* DEFAULT_SUFFIX = "$default";

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

bool is_synthetic_accessor(DexMethod* m) {
  return boost::starts_with(m->get_simple_deobfuscated_name(), ACCESS_PREFIX) ||
         boost::ends_with(m->get_simple_deobfuscated_name(), DEFAULT_SUFFIX);
}

/**
 * A class that ressembles a fun interface class like the one in P1690830372.
 */
bool is_fun_interface_class(const DexClass* cls) {
  if (!klass::maybe_anonymous_class(cls)) {
    return false;
  }
  if (cls->get_super_class() != type::java_lang_Object()) {
    return false;
  }
  if (cls->get_interfaces()->size() != 1) {
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
  size_t cnt = 0;
  const auto* callback_name = vmethods.at(0)->get_name();
  for (const auto* m : vmethods) {
    if (m->get_name() != callback_name) {
      return false;
    }
    if (!is_synthetic(m)) {
      cnt++;
    }
    if (cnt > 1) {
      return false;
    }
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
bool add_annotations(DexMember* member, DexAnnotationSet* anno_set) {
  if (member && member->is_def()) {
    auto def_member = member->as_def();
    auto existing_annos = def_member->get_anno_set();
    if (existing_annos) {
      existing_annos->combine_with(*anno_set);
    } else {
      DexAccessFlags access = def_member->get_access();
      def_member->set_access(ACC_SYNTHETIC);
      def_member->attach_annotation_set(
          std::make_unique<DexAnnotationSet>(*anno_set));
      def_member->set_access(access);
    }
    return true;
  }
  return false;
}

void add_param_annotations(DexMethod* m,
                           DexAnnotationSet* anno_set,
                           int param) {
  if (m->get_param_anno()) {
    if (m->get_param_anno()->count(param) == 1) {
      std::unique_ptr<DexAnnotationSet>& param_anno_set =
          m->get_param_anno()->at(param);
      if (param_anno_set != nullptr) {
        param_anno_set->combine_with(*anno_set);
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

void patch_parameter_from_field(DexMethod* m,
                                IRInstruction* insn,
                                src_index_t arg_index,
                                DexAnnotationSet* anno_set,
                                live_range::UseDefChains* ud_chains) {
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
        add_param_annotations(m, anno_set, param_index);
      }
      param_index += 1;
    }
  }
}

void patch_param_from_method_invoke(
    TypeEnvironments& envs,
    type_inference::TypeInference& inference,
    DexMethod* caller,
    IRInstruction* insn,
    live_range::UseDefChains* ud_chains,
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
      if (patch_accessor) {
        if (anno_type && anno_type == annotation) {
          // Safe assignment. Nothing to do.
          continue;
        }
        DexAnnotationSet anno_set = DexAnnotationSet();
        anno_set.add_annotation(std::make_unique<DexAnnotation>(
            DexType::make_type(annotation.get()->get_name()), DAV_RUNTIME));
        patch_parameter_from_field(caller, insn, arg_index, &anno_set,
                                   ud_chains);
      }
      DexAnnotationSet& param_anno_set = *param_anno.second;
      if (missing_param_annos) {
        missing_param_annos->push_back({arg_index, param_anno_set});
      }
      TRACE(TAC, 2, "Missing param annotation %s in %s", SHOW(&param_anno_set),
            SHOW(caller));
    }
  }
}

void patch_setter_method(type_inference::TypeInference& inference,
                         DexMethod* caller,
                         IRInstruction* insn,
                         live_range::UseDefChains* ud_chains) {
  always_assert(opcode::is_an_iput(insn->opcode()) ||
                opcode::is_an_sput(insn->opcode()));
  auto field_ref = insn->get_field();
  auto field_anno = type_inference::get_typedef_anno_from_member(
      field_ref, inference.get_annotations());

  if (field_anno != boost::none) {
    patch_parameter_from_field(caller, insn, 0,
                               field_ref->as_def()->get_anno_set(), ud_chains);
  }
}

} // namespace

// https://kotlinlang.org/docs/fun-interfaces.html#sam-conversions
// sam conversions appear in Kotlin and provide a more concise way to override
// methods. This method handles sam conversiona and all synthetic methods that
// override methods with return or parameter annotations
bool TypedefAnnoPatcher::patch_synth_methods_overriding_annotated_methods(
    DexMethod* m) {
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
      add_annotations(m, &anno_set);
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
      add_param_annotations(m, &anno_set, param_anno.first);
    }
  }
  return false;
}

void TypedefAnnoPatcher::run(const Scope& scope) {
  walk::parallel::classes(scope, [this](DexClass* cls) {
    for (auto m : cls->get_all_methods()) {

      patch_parameters_and_returns(m);
      patch_synth_methods_overriding_annotated_methods(m);
      if (is_constructor(m) &&
          has_typedef_annos(m->get_param_anno(), m_typedef_annos)) {
        patch_synth_cls_fields_from_ctor_param(m);
      }
      if (is_synthesized_lambda_class(cls) || is_fun_interface_class(cls)) {
        patch_local_var_lambda(m);
      }
    }
  });
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (klass::maybe_anonymous_class(cls) && get_enclosing_method(cls)) {
      patch_enclosed_method(cls);
      patch_ctor_params_from_synth_cls_fields(cls);
    }
  });
}

void TypedefAnnoPatcher::patch_ctor_params_from_synth_cls_fields(
    DexClass* cls) {
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
          add_param_annotations(ctor, &anno_set, param_idx - 2);
        }
      }
    }
  }
}

void patch_synthetic_field_from_local_var_lambda(
    const live_range::UseDefChains& ud_chains,
    IRInstruction* insn,
    const src_index_t src,
    DexAnnotationSet* anno_set) {
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
        add_annotations(original_field, anno_set);
      }
    } else {
      add_annotations(field, anno_set);
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
    const type_inference::TypeInference& inference) {
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
          ud_chains, insn, param_anno.first + 1, &anno_set);
    }
  }
}

// Check if the default method calls a method with annotated parameters.
// If there are annotated parameters, return them, but don't patch them since
// they'll be patched by patch_accessors
void TypedefAnnoPatcher::collect_annos_from_default_method(
    DexMethod* method,
    std::vector<std::pair<src_index_t, DexAnnotationSet&>>&
        missing_param_annos) {
  if (!method) {
    return;
  }
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
  TypeEnvironments& envs = inference.get_type_environments();
  live_range::MoveAwareChains chains(method->get_code()->cfg());
  live_range::UseDefChains ud_chains = chains.get_use_def_chains();

  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (opcode::is_an_invoke(opcode)) {
        auto* caller = insn->get_method()->as_def();
        patch_param_from_method_invoke(envs, inference, caller, insn,
                                       &ud_chains, &missing_param_annos, false);
        auto def_method = resolve_method(caller, insn);
        if (def_method && def_method->get_anno_set()) {
          auto anno_set = def_method->get_anno_set();
          auto return_annotation = type_inference::get_typedef_annotation(
              anno_set->get_annotations(), inference.get_annotations());
          if (return_annotation) {
            add_annotations(caller, anno_set);
          }
        }
      }
    }
  }
}

// If the method name is invoke or onClick and is part of a synth class, check
// if it requires annotated fields. If the method calls a default method, check
// that the default's callee has annotated params. If there are annotated
// params, annotate the field and its class' constructor's parameters
void TypedefAnnoPatcher::patch_local_var_lambda(DexMethod* method) {
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

      // if it's a $default or $access method, get the annotations from its
      // callee
      if (opcode::is_invoke_static(insn->opcode())) {
        DexMethod* static_method = insn->get_method()->as_def();
        if (!static_method || !is_synthetic_accessor(static_method)) {
          continue;
        }
        std::vector<std::pair<src_index_t, DexAnnotationSet&>>
            missing_param_annos;
        collect_annos_from_default_method(static_method, missing_param_annos);
        // Patch missing param annotations
        for (auto& pair : missing_param_annos) {
          patch_synthetic_field_from_local_var_lambda(ud_chains, insn,
                                                      pair.first, &pair.second);
        }
      } else if (opcode::is_invoke_interface(insn->opcode())) {
        auto* callee_def = resolve_method(method, insn);
        auto callees =
            mog::get_overriding_methods(m_method_override_graph, callee_def);
        for (auto callee : callees) {
          annotate_local_var_field_from_callee(callee, insn, ud_chains,
                                               inference);
        }
      } else if (opcode::is_an_invoke(insn->opcode())) {
        const DexMethod* callee = insn->get_method()->as_def();
        annotate_local_var_field_from_callee(callee, insn, ud_chains,
                                             inference);
      }
    }
  }
}

// Given a constructor of a synthetic class, check if it has typedef annotated
// parameters. If it does, find the field that the parameter got put into and
// annotate it.
void TypedefAnnoPatcher::patch_synth_cls_fields_from_ctor_param(
    DexMethod* ctor) {
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
      if (!opcode::is_an_iput(insn->opcode())) {
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
        add_annotations(field, &anno_set);
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
          patch_parameters_and_returns(getter_method->as_def());
        }
        auto setter_method = DexMethod::get_method(
            class_name_dot + "set" + field_name + ":(" + int_or_string + ")V");
        if (setter_method) {
          patch_parameters_and_returns(setter_method->as_def());
        }
      }
    }
  }
}

void TypedefAnnoPatcher::patch_enclosed_method(DexClass* cls) {
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
        add_annotations(dex_field, &a_set);
      }
    }
  }
}

void TypedefAnnoPatcher::patch_first_level_nested_lambda(DexClass* cls) {
  auto enclosing_method = get_enclosing_method(cls);
  // if the class is not enclosed, there is no annotation to derive
  if (!enclosing_method) {
    return;
  }
  // if the parent class is anonymous or not a def, there is no annotation to
  // derive. If an annotation is needed, it will be propagated later in
  // patch_enclosed_method
  const auto parent_class = type_class(enclosing_method->get_class());
  if (klass::maybe_anonymous_class(parent_class) ||
      !enclosing_method->is_def()) {
    return;
  }
  // In Java, the common class name is everything before the first $,
  // and there is no second $ in the class name. For example, from the
  // tests, the method name is
  // Lcom/facebook/redextest/TypedefAnnoCheckerTest$2;.override_method:()V
  //
  // In kotlin, the common class name is everything before the second $
  // From the tests, the method name is
  // Lcom/facebook/redextest/TypedefAnnoCheckerKtTest$testLambdaCall$1;.invoke:()Ljava/lang/String;
  auto cls_name = cls->get_deobfuscated_name_or_empty_copy();
  auto common_class_name_end = cls_name.find('$') + 1;
  if (cls_name[common_class_name_end] < '0' ||
      cls_name[common_class_name_end] > '9') {
    common_class_name_end = cls_name.find('$', cls_name.find('$') + 1);
  }
  if (common_class_name_end == std::string::npos) {
    return;
  }

  DexMethod* method = enclosing_method->as_def();
  IRCode* code = method->get_code();
  if (!code) {
    return;
  }

  always_assert_log(code->editable_cfg_built(), "%s has no cfg built",
                    SHOW(method));
  auto& cfg = code->cfg();
  if (!method->get_param_anno()) {
    // Method does not pass in any typedef values to synthetic constructor.
    // Nothing to do.
    // TODO: if a method calls the synthetic constructor with an annotated value
    // from another method's return, the annotation can be derived and should be
    // patched
    return;
  }

  type_inference::TypeInference inference(cfg, false, m_typedef_annos,
                                          &m_method_override_graph);

  bool has_typedef_annotated_params = false;
  for (auto const& param_anno : *method->get_param_anno()) {
    auto annotation = type_inference::get_typedef_annotation(
        param_anno.second->get_annotations(), inference.get_annotations());
    if (annotation != boost::none) {
      has_typedef_annotated_params = true;
    }
  }
  if (!has_typedef_annotated_params) {
    return;
  }

  // If the original method calls a synthetic constructor with typedef params,
  // add the correct annotations to the params, so we can find the correct
  // synthetic fields that need to be patched
  inference.run(method);
  TypeEnvironments& envs = inference.get_type_environments();
  bool patched_params = false;
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      if (insn->opcode() != OPCODE_INVOKE_DIRECT) {
        continue;
      }
      // if the method invoked is not a constructor of the synthetic class
      // we're currently analyzing, skip it
      DexMethod* method_def = insn->get_method()->as_def();
      if (!method_def || !is_constructor(method_def) ||
          method_def->get_class()->get_name() != cls->get_name()) {
        continue;
      }
      // patch the constructor's parameters
      size_t total_args = method_def->get_proto()->get_args()->size();
      size_t src_idx = 1;
      while (src_idx <= total_args) {
        auto param_anno = envs.at(insn).get_annotation(insn->src(src_idx));
        if (param_anno != boost::none) {
          auto anno_set = DexAnnotationSet();
          anno_set.add_annotation(std::make_unique<DexAnnotation>(
              DexType::make_type(param_anno.get()->get_name()),
              DexAnnotationVisibility::DAV_RUNTIME));
          add_param_annotations(method_def, &anno_set, src_idx - 1);
          patched_params = true;
        }
        src_idx += 1;
      }
    }
  }
  if (!patched_params) {
    return;
  }

  // Patch the field and store it in annotated_fields so any synthetic
  // classes that are derived from the current one don't need to traverse
  // up to the non-synthetic class to get the typedef annotation.
  // Further derived classes will have the same prefix class name and field
  // that needs to be annotated
  std::vector<const DexField*> annotated_fields;
  for (auto ctor : cls->get_ctors()) {
    IRCode* ctor_code = ctor->get_code();
    auto& ctor_cfg = ctor_code->cfg();
    type_inference::TypeInference ctor_inference(
        ctor_cfg, false, m_typedef_annos, &m_method_override_graph);
    ctor_inference.run(ctor);
    TypeEnvironments& ctor_envs = ctor_inference.get_type_environments();

    live_range::MoveAwareChains chains(ctor_cfg);
    live_range::UseDefChains ud_chains = chains.get_use_def_chains();
    for (cfg::Block* b : ctor_cfg.blocks()) {
      for (auto& mie : InstructionIterable(b)) {
        auto* insn = mie.insn;
        if (!opcode::is_an_iput(insn->opcode())) {
          continue;
        }
        auto& env = ctor_envs.at(insn);
        if (!typedef_anno::is_int(env, insn->src(0)) &&
            !typedef_anno::is_string(env, insn->src(0))) {
          continue;
        }
        auto field = insn->get_field()->as_def();
        if (!field) {
          continue;
        }

        live_range::Use use_of_id{insn, 0};
        auto udchains_it = ud_chains.find(use_of_id);
        auto defs_set = udchains_it->second;
        for (IRInstruction* def : defs_set) {
          auto param_anno = env.get_annotation(def->dest());
          if (param_anno != boost::none) {
            DexAnnotationSet anno_set = DexAnnotationSet();
            anno_set.add_annotation(std::make_unique<DexAnnotation>(
                DexType::make_type(param_anno.get()->get_name()), DAV_RUNTIME));
            add_annotations(field, &anno_set);
            annotated_fields.push_back(field);
          }
        }
      }
    }
  }
  // if the map is already filled in, don't fill it in again.
  // class_prefix is the entire class name before the second dollar sign
  auto class_prefix = cls_name.substr(0, common_class_name_end);
  if (m_lambda_anno_map.get(class_prefix)) {
    return;
  }

  m_lambda_anno_map.emplace(class_prefix, annotated_fields);
}

// This does 3 things:
// 1. if a parameter is passed into an invoked method that expects an annotated
// argument, patch the parameter
// 2. if a parameter is passed into a field write and the field is annotated,
// patch the parameter
// 3. if all method returns are annotated as per TypeInference, patch the method
void TypedefAnnoPatcher::patch_parameters_and_returns(DexMethod* m) {
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
  bool patch_return = true;
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (opcode::is_an_invoke(opcode)) {
        patch_param_from_method_invoke(envs, inference, m, insn, &ud_chains);
      } else if (opcode::is_an_iput(opcode) || opcode::is_an_sput(opcode)) {
        patch_setter_method(inference, m, insn, &ud_chains);
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
    add_annotations(m, &anno_set);
  }
}
