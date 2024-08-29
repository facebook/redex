/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"

#include "AnnoUtils.h"
#include "ClassUtil.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

constexpr const char* ACCESS_PREFIX = "access$";
constexpr const char* DEFAULT_SUFFIX = "$default";
constexpr const char* ANNOTATIONS_SUFFIX = "$annotations";
constexpr const char* COMPANION_SUFFIX = "$cp";
constexpr const char* COMPANION_CLASS = "$Companion";
constexpr const char* PRIVATE_SUFFIX = "$p";

namespace {

bool is_int(const type_inference::TypeEnvironment& env, reg_t reg) {
  return !env.get_int_type(reg).is_top() && !env.get_int_type(reg).is_bottom();
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

bool is_synthetic_kotlin_annotations_method(DexMethod* m) {
  return boost::ends_with(m->get_simple_deobfuscated_name(),
                          ANNOTATIONS_SUFFIX);
}

bool is_lamdda_callback(DexMethod* m) {
  return m->get_simple_deobfuscated_name() == "invoke" ||
         m->get_simple_deobfuscated_name() == "onClick";
}

bool has_kotlin_default_ctor_marker(DexMethod* m) {
  auto params = m->get_proto()->get_args();
  if (params->size() > 1 &&
      params->at(params->size() - 1)->str() ==
          "Lkotlin/jvm/internal/DefaultConstructorMarker;") {
    return true;
  }
  return false;
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

DexField* lookup_property_field(DexMethod* m) {
  std::basic_string<char> field_name;
  auto method_name = m->get_simple_deobfuscated_name();
  const auto method_name_len = method_name.length();

  if (boost::starts_with(method_name, "get") ||
      boost::starts_with(method_name, "set")) {
    if (method_name_len <= 3) {
      return nullptr;
    }
    // getSomeField -> SomeField
    field_name = method_name.substr(3);
    // SomeField -> someField
    field_name.at(0) = std::tolower(field_name.at(0));
  } else if (boost::starts_with(method_name, ACCESS_PREFIX) &&
             boost::ends_with(method_name, COMPANION_SUFFIX)) {
    if (method_name_len <= (7 + 3)) {
      return nullptr;
    }

    // access$getBLOKS_RENDERING_TYPE$cp -> getBLOKS_RENDERING_TYPE
    field_name = method_name.substr(7, method_name_len - (7 + 3));
    // getBLOKS_RENDERING_TYPE -> BLOKS_RENDERING_TYPE
    field_name = field_name.substr(3);
  } else if (boost::starts_with(method_name, ACCESS_PREFIX) &&
             boost::ends_with(method_name, PRIVATE_SUFFIX)) {
    if (method_name_len <= (7 + 2)) {
      return nullptr;
    }

    // access$getUiSection$p -> getUiSection
    field_name = method_name.substr(7, method_name_len - (7 + 2));
    // getUiSection -> uiSection
    field_name = field_name.substr(3);
    field_name.at(0) = std::tolower(field_name.at(0));
  } else {
    return nullptr;
  }

  std::string_view int_or_string;
  if (boost::starts_with(m->get_simple_deobfuscated_name(), "set")) {
    auto args = m->get_proto()->get_args();
    if (args->empty()) {
      return nullptr;
    }
    DexType* param_type = m->get_proto()->get_args()->at(0);
    if (!type::is_int(param_type) && param_type != type::java_lang_String()) {
      return nullptr;
    }
    int_or_string = type::is_int(param_type)
                        ? "I"
                        : type::java_lang_String()->get_name()->str();
  } else {
    const auto* rtype = m->get_proto()->get_rtype();
    if (!type::is_int(rtype) && rtype != type::java_lang_String()) {
      return nullptr;
    }
    int_or_string =
        type::is_int(rtype) ? "I" : type::java_lang_String()->get_name()->str();
  }

  auto class_name_dot = m->get_class()->get_name()->str() + ".";
  auto* fref =
      DexField::get_field(class_name_dot + field_name + ":" + int_or_string);
  return fref && fref->is_def() ? fref->as_def() : nullptr;
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

void collect_param_anno_from_instruction(
    TypeEnvironments& envs,
    type_inference::TypeInference& inference,
    DexMethod* caller,
    IRInstruction* insn,
    std::vector<std::pair<src_index_t, DexAnnotationSet&>>& missing_param_annos,
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
      int param_index = insn->opcode() == OPCODE_INVOKE_STATIC
                            ? param_anno.first
                            : param_anno.first + 1;
      reg_t param_reg = insn->src(param_index);
      auto anno_type = env.get_annotation(param_reg);
      if (patch_accessor && anno_type && anno_type == annotation) {
        // Safe assignment. Nothing to do.
        continue;
      }
      DexAnnotationSet& param_anno_set = *param_anno.second;
      missing_param_annos.push_back({param_index, param_anno_set});
      TRACE(TAC, 2, "Missing param annotation %s in %s", SHOW(&param_anno_set),
            SHOW(caller));
    }
  }
  if (def_method->get_anno_set()) {
    auto return_annotation = type_inference::get_typedef_annotation(
        def_method->get_anno_set()->get_annotations(),
        inference.get_annotations());
    if (return_annotation) {
      add_annotations(caller, def_method->get_anno_set());
    }
  }
}

void patch_return_anno_from_get(type_inference::TypeInference& inference,
                                DexMethod* caller,
                                IRInstruction* insn) {
  always_assert(opcode::is_an_iget(insn->opcode()) ||
                opcode::is_an_sget(insn->opcode()));
  auto name = caller->get_deobfuscated_name_or_empty();
  auto pos = name.rfind('$');
  if (pos == std::string::npos) {
    return;
  }
  pos++;
  if (!(pos < name.size() && name[pos] >= '0' && name[pos] <= '9')) {
    return;
  }
  auto field_ref = insn->get_field();
  auto field_anno = type_inference::get_typedef_anno_from_member(
      field_ref, inference.get_annotations());

  if (field_anno != boost::none) {
    // Patch missing return annotations from accessed fields
    caller->attach_annotation_set(std::make_unique<DexAnnotationSet>(
        *field_ref->as_def()->get_anno_set()));
  }
}

} // namespace

// check if the field has any typedef annotations. If it does, patch the method
// return if it's a getter or the parameter if it's a setter
void SynthAccessorPatcher::try_adding_annotation_to_accessor(
    DexMethod* m, const DexField* field) {
  always_assert(field != nullptr);
  auto anno =
      type_inference::get_typedef_anno_from_member(field, m_typedef_annos);
  if (anno == boost::none) {
    return;
  }

  DexAnnotationSet anno_set = DexAnnotationSet();
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      DexType::make_type(anno.get()->get_name()), DAV_RUNTIME));

  // annotate the parameter
  if (boost::starts_with(m->get_simple_deobfuscated_name(), "set") ||
      boost::starts_with(m->get_simple_deobfuscated_name(), "access$set")) {
    size_t param_index = 0;
    if (boost::ends_with(m->get_simple_deobfuscated_name(), PRIVATE_SUFFIX)) {
      param_index = 1;
    }
    if (m->get_param_anno()) {
      m->get_param_anno()
          ->at(param_index)
          ->add_annotation(std::make_unique<DexAnnotation>(
              DexType::make_type(anno.get()->get_name()), DAV_RUNTIME));
    } else {
      DexAccessFlags access = m->get_access();
      m->set_access(ACC_SYNTHETIC);
      m->attach_param_annotation_set(
          param_index, std::make_unique<DexAnnotationSet>(anno_set));
      m->set_access(access);
    }
  } else {
    add_annotations(m, &anno_set);
  }
}

void SynthAccessorPatcher::patch_kotlin_annotated_property_getter_setter(
    DexMethod* m) {
  if (!boost::starts_with(m->get_simple_deobfuscated_name(), "get") &&
      !boost::starts_with(m->get_simple_deobfuscated_name(), "set")) {
    return;
  }

  const auto* property_field = lookup_property_field(m);
  if (property_field == nullptr) {
    return;
  }
  try_adding_annotation_to_accessor(m, property_field);
}

/*
 * A synthesized Kotlin method like access$getBLOKS_RENDERING_TYPE$cp(); that
 * enables access to private property for Kotlin Companion property.
 */
void SynthAccessorPatcher::patch_kotlin_companion_property_accessor(
    DexMethod* m) {
  if (!boost::starts_with(m->get_simple_deobfuscated_name(), ACCESS_PREFIX) ||
      !boost::ends_with(m->get_simple_deobfuscated_name(), COMPANION_SUFFIX)) {
    return;
  }

  const auto* property_field = lookup_property_field(m);
  if (property_field == nullptr) {
    return;
  }
  try_adding_annotation_to_accessor(m, property_field);
}

/*
 * A synthesized Kotlin method like access$getUiSection$p(); that enables access
 * to private property on the class.
 */
void SynthAccessorPatcher::patch_kotlin_property_private_getter(DexMethod* m) {
  if (!boost::starts_with(m->get_simple_deobfuscated_name(), ACCESS_PREFIX) ||
      !boost::ends_with(m->get_simple_deobfuscated_name(), PRIVATE_SUFFIX)) {
    return;
  }

  const auto* property_field = lookup_property_field(m);
  if (property_field == nullptr) {
    return;
  }
  try_adding_annotation_to_accessor(m, property_field);
}

void SynthAccessorPatcher::run(const Scope& scope) {
  walk::parallel::methods(scope, [this](DexMethod* m) {
    patch_kotlin_annotated_property_getter_setter(m);
    if (is_synthetic_accessor(m)) {
      collect_accessors(m);
    }
    patch_kotlin_companion_property_accessor(m);
    patch_kotlin_property_private_getter(m);
    if (is_synthetic_kotlin_annotations_method(m)) {
      patch_kotlin_annotations(m);
    }
    if (is_constructor(m)) {
      if (m->get_param_anno()) {
        patch_synth_cls_fields_from_ctor_param(m);
      } else {
        if (has_kotlin_default_ctor_marker(m)) {
          collect_accessors(m);
        }
      }
    }
    if (is_lamdda_callback(m) &&
        klass::maybe_anonymous_class(type_class(m->get_class()))) {
      patch_local_var_lambda(m);
    }
  });
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (klass::maybe_anonymous_class(cls) && get_enclosing_method(cls)) {
      patch_enclosed_method(cls);
      patch_ctor_params_from_synth_cls_fields(cls);
    }
  });
}

void SynthAccessorPatcher::patch_ctor_params_from_synth_cls_fields(
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
        if (!is_int(env, insn->dest()) && !is_string(env, insn->dest())) {
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
          DexAccessFlags access = ctor->get_access();
          ctor->set_access(ACC_SYNTHETIC);
          ctor->attach_param_annotation_set(
              param_idx - 2, std::make_unique<DexAnnotationSet>(anno_set));
          ctor->set_access(access);
        }
      }
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
      live_range::Use use_of_id{insn, (src_index_t)(param_anno.first + 1)};
      auto udchains_it = ud_chains.find(use_of_id);
      auto defs_set = udchains_it->second;
      for (IRInstruction* def : defs_set) {
        if (!opcode::is_an_iget(def->opcode())) {
          continue;
        }
        auto field = def->get_field()->as_def();
        if (!field) {
          continue;
        }
        DexAnnotationSet anno_set = DexAnnotationSet();
        anno_set.add_annotation(std::make_unique<DexAnnotation>(
            DexType::make_type(annotation.get()->get_name()), DAV_RUNTIME));
        add_annotations(field, &anno_set);
      }
    }
  }
}

// Check if the default method calls a method with annotated parameters.
// If there are annotated parameters, return them, but don't patch them since
// they'll be patched by collect_accessors
void SynthAccessorPatcher::collect_annos_from_default_method(
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

  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (opcode::is_an_invoke(opcode)) {
        collect_param_anno_from_instruction(envs, inference,
                                            insn->get_method()->as_def(), insn,
                                            missing_param_annos, false);
      }
    }
  }
}

// If the method name is invoke or onClick and is part of a synth class, check
// if it requires annotated fields. If the method calls a default method, check
// that the default's callee has annotated params. If there are annotated
// params, annotate the field and its class' constructor's parameters
void SynthAccessorPatcher::patch_local_var_lambda(DexMethod* method) {
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
          DexAnnotationSet anno_set = pair.second;
          live_range::Use use_of_id{insn, pair.first};
          auto udchains_it = ud_chains.find(use_of_id);
          auto defs_set = udchains_it->second;
          for (IRInstruction* def : defs_set) {
            if (!opcode::is_an_iget(def->opcode())) {
              continue;
            }
            auto field = def->get_field()->as_def();
            if (!field) {
              continue;
            }
            add_annotations(field, &anno_set);
          }
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
void SynthAccessorPatcher::patch_synth_cls_fields_from_ctor_param(
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
      if (!is_int(env, insn->src(0)) && !is_string(env, insn->src(0))) {
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
        add_annotations(
            DexMethod::get_method(class_name_dot + "get" + field_name + ":()" +
                                  int_or_string),
            &anno_set);
        add_annotations(
            DexMethod::get_method(class_name_dot + "set" + field_name + ":(" +
                                  int_or_string + ")V"),
            &anno_set);
      }
    }
  }
}

void SynthAccessorPatcher::patch_enclosed_method(DexClass* cls) {
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

void SynthAccessorPatcher::patch_first_level_nested_lambda(DexClass* cls) {
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
          DexAccessFlags access = method_def->get_access();
          method_def->set_access(ACC_SYNTHETIC);
          method_def->attach_param_annotation_set(
              src_idx - 1, std::make_unique<DexAnnotationSet>(anno_set));
          method_def->set_access(access);
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
        if (!is_int(env, insn->src(0)) && !is_string(env, insn->src(0))) {
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

void SynthAccessorPatcher::patch_kotlin_annotations(DexMethod* m) {
  IRCode* code = m->get_code();
  if (!code) {
    return;
  }

  DexAnnotationSet* anno_set = m->get_anno_set();
  if (!anno_set) {
    return;
  }
  DexType* safe_annotation = nullptr;
  bool has_typedef = false;
  for (auto const& anno : anno_set->get_annotations()) {
    auto const anno_class = type_class(anno->type());
    if (!anno_class) {
      continue;
    }
    for (auto safe_anno : m_typedef_annos) {
      if (get_annotation(anno_class, safe_anno)) {
        if (has_typedef) {
          always_assert_log(
              false,
              "Method %s cannot have more than one TypeDef annotation",
              SHOW(m));
          return;
        }
        has_typedef = true;
        safe_annotation = safe_anno;
      }
    }
  }
  if (!safe_annotation) {
    return;
  }
  // example method name:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.getField_three$annotations:()V
  // getter:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.getField_three:()Ljava/lang/String;
  // setter:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.setField_three:(Ljava/lang/String;)V;
  // field is one of:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.Field_three:Ljava/lang/String;
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.field_three:Ljava/lang/String;
  // companion example
  // companion method:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest$Companion;.getField_one$annotations:()V
  // getters:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest$Companion.getField_one:()I
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.access$getField_one$cp:()I
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.access$getField_one$p:()I
  // setters:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest$Companion.setField_one:(I)
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.access$setField_one$cp:(I)V
  // field is one of:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.Field_one:I
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.field_one:I

  // some synthetic interfaces' names have $-CC. Delete it from the name
  auto original_class_name = m->get_class()->get_name()->str();
  auto class_name = original_class_name + ".";
  auto pos = class_name.find("$-CC");
  if (pos != std::string::npos) class_name.erase(pos, 4);
  auto companion_pos = class_name.find(COMPANION_CLASS);
  auto base_class_name = (companion_pos != std::string::npos)
                             ? class_name.substr(0, companion_pos) + ";."
                             : class_name;

  auto anno_method_name = m->get_simple_deobfuscated_name();
  auto method_name =
      anno_method_name.substr(0, anno_method_name.find(ANNOTATIONS_SUFFIX));
  auto int_or_string = safe_annotation->get_name()->str() ==
                               "Lcom/facebook/redex/annotations/SafeStringDef;"
                           ? type::java_lang_String()->get_name()->str()
                           : "I";
  // we need to remove the first three characters, 'get', from the annotations
  // methoid name to derive the field name
  auto field_name = method_name.substr(3, method_name.size());

  // add annotations to getter and setter methods
  add_annotations(
      DexMethod::get_method(class_name + method_name + ":()" + int_or_string),
      anno_set);
  add_annotations(DexMethod::get_method(class_name + "set" + field_name + ":(" +
                                        int_or_string + ")V"),
                  anno_set);

  // add annotations to access non-companion getter and setter methods
  add_annotations(
      DexMethod::get_method(base_class_name + ACCESS_PREFIX + "get" +
                            field_name + "$cp:()" + int_or_string),
      anno_set);
  add_annotations(
      DexMethod::get_method(base_class_name + ACCESS_PREFIX + "set" +
                            field_name + "$cp:(" + int_or_string + ")V"),
      anno_set);
  add_annotations(DexMethod::get_method(
                      base_class_name + ACCESS_PREFIX + "get" + field_name +
                      "$p:(" + original_class_name + ")" + int_or_string),
                  anno_set);

  // add annotations to field
  if (!add_annotations(DexField::get_field(base_class_name + field_name + ":" +
                                           int_or_string),
                       anno_set)) {
    field_name.at(0) = std::tolower(field_name.at(0));
    add_annotations(
        DexField::get_field(base_class_name + field_name + ":" + int_or_string),
        anno_set);
  }
}

void SynthAccessorPatcher::collect_accessors(DexMethod* m) {
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
  std::vector<std::pair<src_index_t, DexAnnotationSet&>> missing_param_annos;
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (opcode::is_an_invoke(opcode)) {
        collect_param_anno_from_instruction(envs, inference, m, insn,
                                            missing_param_annos);
      } else if (opcode::is_an_iget(opcode) || opcode::is_an_sget(opcode)) {
        patch_return_anno_from_get(inference, m, insn);
      }
    }
  }

  // Patch missing param annotations
  for (auto& pair : missing_param_annos) {
    int param_index = pair.first;
    if (is_synthetic_accessor(m)) {
      always_assert(is_static(m));
    } else {
      always_assert(is_constructor(m));
      param_index -= 1;
    }
    m->attach_param_annotation_set(
        param_index, std::make_unique<DexAnnotationSet>(pair.second));
    TRACE(TAC, 2, "Add param annotation %s at %d to %s", SHOW(&pair.second),
          param_index, SHOW(m));
  }
}

void TypedefAnnoChecker::run(DexMethod* m) {
  IRCode* code = m->get_code();
  if (!code) {
    return;
  }

  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  std::unordered_set<DexType*> anno_set;
  anno_set.emplace(m_config.int_typedef);
  anno_set.emplace(m_config.str_typedef);
  type_inference::TypeInference inference(cfg, false, anno_set,
                                          &m_method_override_graph);
  inference.run(m);

  live_range::MoveAwareChains chains(cfg);
  live_range::UseDefChains ud_chains = chains.get_use_def_chains();

  boost::optional<const DexType*> return_annotation = boost::none;
  DexAnnotationSet* return_annos = m->get_anno_set();
  if (return_annos) {
    return_annotation = type_inference::get_typedef_annotation(
        return_annos->get_annotations(), inference.get_annotations());
  }
  TypeEnvironments& envs = inference.get_type_environments();
  TRACE(TAC, 2, "Start checking %s", SHOW(m));
  TRACE(TAC, 5, "%s", SHOW(cfg));
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;

      check_instruction(m, &inference, insn, return_annotation, &ud_chains,
                        envs);
    }
  }
  if (!m_good) {
    TRACE(TAC, 2, "Done checking %s", SHOW(m));
  }
}

void TypedefAnnoChecker::check_instruction(
    DexMethod* m,
    const type_inference::TypeInference* inference,
    IRInstruction* insn,
    const boost::optional<const DexType*>& return_annotation,
    live_range::UseDefChains* ud_chains,
    TypeEnvironments& envs) {
  // if the invoked method's arguments have annotations with the
  // @SafeStringDef or @SafeIntDef annotation, check that TypeInference
  // inferred the correct annotation for the values being passed in
  auto& env = envs.find(insn)->second;
  IROpcode opcode = insn->opcode();
  switch (opcode) {
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    auto* callee_def = resolve_method(m, insn);
    if (!callee_def) {
      return;
    }
    std::vector<const DexMethod*> callees;
    if (mog::is_true_virtual(m_method_override_graph, callee_def) &&
        !callee_def->get_code()) {
      callees =
          mog::get_overriding_methods(m_method_override_graph, callee_def);
    }
    callees.push_back(callee_def);
    for (const DexMethod* callee : callees) {
      if (!callee->get_param_anno()) {
        // Callee does not expect any Typedef value. Nothing to do.
        return;
      }
      for (auto const& param_anno : *callee->get_param_anno()) {
        auto annotation = type_inference::get_typedef_annotation(
            param_anno.second->get_annotations(), inference->get_annotations());
        if (annotation == boost::none) {
          continue;
        }
        int param_index = insn->opcode() == OPCODE_INVOKE_STATIC
                              ? param_anno.first
                              : param_anno.first + 1;
        reg_t reg = insn->src(param_index);
        auto anno_type = env.get_annotation(reg);
        auto type = env.get_dex_type(reg);

        // TypeInference inferred a different annotation
        if (anno_type && anno_type != annotation) {
          std::ostringstream out;
          if (anno_type.value() == type::java_lang_Object()) {
            out << "TypedefAnnoCheckerPass: while invoking " << show(callee)
                << "\n in method " << show(m) << "\n parameter "
                << param_anno.first << "should have the annotation "
                << annotation.value()->get_name()->c_str()
                << "\n but it instead contains an ambiguous annotation, "
                   "implying that the parameter was joined with another "
                   "typedef annotation \n before the method invokation. The "
                   "ambiguous annotation is unsafe, and typedef annotations "
                   "should not be mixed.\n"
                << " failed instruction: " << show(insn) << "\n\n";
          } else {
            out << "TypedefAnnoCheckerPass: while invoking " << show(callee)
                << "\n in method " << show(m) << "\n parameter "
                << param_anno.first << " has the annotation " << show(anno_type)
                << "\n but the method expects the annotation to be "
                << annotation.value()->get_name()->c_str()
                << ".\n failed instruction: " << show(insn) << "\n\n";
          }
          m_error += out.str();
          m_good = false;
        } else if (is_not_str_nor_int(env, reg)) {
          std::ostringstream out;
          out << "TypedefAnnoCheckerPass: the annotation " << show(annotation)
              << "\n annotates a parameter with an incompatible type "
              << show(type) << "\n or a non-constant parameter in method "
              << show(m) << "\n while trying to invoke the method "
              << show(callee) << ".\n failed instruction: " << show(insn)
              << "\n\n";
          m_error += out.str();
          m_good = false;
        } else if (!anno_type) {
          // TypeInference didn't infer anything
          bool good = check_typedef_value(m, annotation, ud_chains, insn,
                                          param_index, inference, envs);
          if (!good) {
            std::ostringstream out;
            out << " Error invoking " << show(callee) << "\n";
            out << " Incorrect parameter's index: " << param_index << "\n\n";
            m_error += out.str();
            TRACE(TAC, 1, "invoke method: %s", SHOW(callee));
          }
        }
      }
    }
    break;
  }
  // when writing to annotated fields, check that the value is annotated
  case OPCODE_IPUT:
  case OPCODE_SPUT:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_IPUT_OBJECT: {
    auto env_anno = env.get_annotation(insn->src(0));
    auto field_anno = type_inference::get_typedef_anno_from_member(
        insn->get_field(), inference->get_annotations());
    if (env_anno != boost::none && field_anno != boost::none &&
        env_anno.value() != field_anno.value()) {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: The method " << show(m)
          << "\n assigned a field " << insn->get_field()->c_str()
          << "\n with annotation " << show(field_anno)
          << "\n to a value with annotation " << show(env_anno)
          << ".\n failed instruction: " << show(insn) << "\n\n";
      m_error += out.str();
      m_good = false;
    }
    break;
  }
  // if there's an annotation that has a string typedef or an int typedef
  // annotation in the method's signature, check that TypeInference
  // inferred that annotation in the retured value
  case OPCODE_RETURN:
  case OPCODE_RETURN_OBJECT: {
    if (return_annotation) {
      reg_t reg = insn->src(0);
      auto anno_type = env.get_annotation(reg);
      if (anno_type && anno_type != return_annotation) {
        std::ostringstream out;
        if (anno_type.value() == type::java_lang_Object()) {
          out << "TypedefAnnoCheckerPass: The method " << show(m)
              << "\n has an annotation "
              << return_annotation.value()->get_name()->c_str()
              << "\n in its method signature, but the returned value has an "
                 "ambiguous annotation, implying that the value was joined \n"
                 "with another typedef annotation within the method. The "
                 "ambiguous annotation is unsafe, \nand typedef annotations "
                 "should not be mixed. \n"
              << "failed instruction: " << show(insn) << "\n\n";
        } else {
          out << "TypedefAnnoCheckerPass: The method " << show(m)
              << "\n has an annotation "
              << return_annotation.value()->get_name()->c_str()
              << "\n in its method signature, but the returned value "
                 "contains the annotation \n"
              << show(anno_type) << " instead.\n"
              << " failed instruction: " << show(insn) << "\n\n";
        }
        m_error += out.str();
        m_good = false;
      } else if (is_not_str_nor_int(env, reg)) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the annotation "
            << show(return_annotation)
            << "\n annotates a value with an incompatible type or a "
               "non-constant value in method\n "
            << show(m) << " .\n"
            << " failed instruction: " << show(insn) << "\n\n";
        m_error += out.str();
        m_good = false;
      } else if (!anno_type) {
        bool good = check_typedef_value(m, return_annotation, ud_chains, insn,
                                        0, inference, envs);
        if (!good) {
          std::ostringstream out;
          out << " Error caught when returning the faulty value\n\n";
          m_error += out.str();
        }
      }
    }
    break;
  }
  default:
    break;
  }
}

bool TypedefAnnoChecker::check_typedef_value(
    DexMethod* m,
    const boost::optional<const DexType*>& annotation,
    live_range::UseDefChains* ud_chains,
    IRInstruction* insn,
    const src_index_t src,
    const type_inference::TypeInference* inference,
    TypeEnvironments& envs) {

  auto anno_class = type_class(annotation.value());
  const auto* str_value_set = m_strdef_constants.get_unsafe(anno_class);
  const auto* int_value_set = m_intdef_constants.get_unsafe(anno_class);

  bool has_str_vals = str_value_set != nullptr && !str_value_set->empty();
  bool has_int_vals = int_value_set != nullptr && !int_value_set->empty();
  always_assert_log(has_int_vals ^ has_str_vals,
                    "%s has both str and int const values", SHOW(anno_class));
  if (!has_str_vals && !has_int_vals) {
    TRACE(TAC, 1, "%s contains no annotation constants", SHOW(anno_class));
    return true;
  }

  live_range::Use use_of_id{insn, src};
  auto udchains_it = ud_chains->find(use_of_id);
  auto defs_set = udchains_it->second;

  for (IRInstruction* def : defs_set) {
    switch (def->opcode()) {
    case OPCODE_CONST_STRING: {
      auto const const_value = def->get_string();
      if (str_value_set->count(const_value) == 0) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << show(m)
            << "\n the string value " << show(const_value)
            << " does not have the typedef annotation \n"
            << show(annotation)
            << " attached to it. \n Check that the value is annotated and "
               "exists in the typedef annotation class.\n"
            << " failed instruction: " << show(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
      }
      break;
    }
    case OPCODE_CONST: {
      auto const const_value = def->get_literal();
      if (has_str_vals && const_value == 0) {
        // Null assigned to a StringDef value. This is valid. We don't enforce
        // nullness.
        break;
      }
      if (int_value_set->count(const_value) == 0) {
        // when passing an integer to a default method, the value will be 0 if
        // the default method will the default value. The const 0 is not
        // annotated and might not be in the IntDef. Since the checker will
        // check that the default value is a member of the IntDef, passing in 0
        // is safe. Example caller and default methods: P1222824190 P1222829651
        if (const_value == 0 && opcode::is_an_invoke(insn->opcode())) {
          DexMethodRef* callee = insn->get_method();
          if (callee->is_def() &&
              boost::ends_with(callee->as_def()->get_simple_deobfuscated_name(),
                               DEFAULT_SUFFIX)) {
            break;
          }
        }
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << show(m)
            << "\n the int value " << show(const_value)
            << " does not have the typedef annotation \n"
            << show(annotation)
            << " attached to it. \n Check that the value is annotated and "
               "exists in its typedef annotation class.\n"
            << " failed instruction: " << show(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
      }
      break;
    }
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM: {
      // this is for cases similar to testIfElseParam in the integ tests
      // where the boolean parameter undergoes an OPCODE_MOVE and
      // gets returned instead of one of the two ints
      auto env = envs.find(def);
      if (env->second.get_int_type(def->dest()).element() ==
          (IntType::BOOLEAN)) {
        if (int_value_set->count(0) == 0 || int_value_set->count(1) == 0) {
          std::ostringstream out;
          out << "TypedefAnnoCheckerPass: the method" << show(m)
              << "\n assigns a int with typedef annotation " << show(annotation)
              << "\n to either 0 or 1, which is invalid because the typedef "
                 "annotation class does not contain both the values 0 and 1.\n"
              << " failed instruction: " << show(def) << "\n";
          m_good = false;
          return false;
        }
        break;
      }
      auto anno = env->second.get_annotation(def->dest());
      if (anno == boost::none || anno != annotation) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << show(m)
            << "\n one of the parameters needs to have the typedef annotation "
            << show(annotation)
            << "\n attached to it. Check that the value is annotated and "
               "exists in the typedef annotation class.\n"
            << " failed instruction: " << show(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
      }
      break;
    }
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_INTERFACE: {
      auto def_method = resolve_method(m, def);
      if (!def_method) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in the method " << show(m)
            << "\n the source of the value with annotation " << show(annotation)
            << "\n is produced by invoking an unresolveable callee, so the "
               "value safety is not guaranteed.\n"
            << " failed instruction: " << show(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
      }
      std::vector<const DexMethod*> callees;
      if (mog::is_true_virtual(m_method_override_graph, def_method) &&
          !def_method->get_code()) {
        callees =
            mog::get_overriding_methods(m_method_override_graph, def_method);
      }
      callees.push_back(def_method);
      for (const DexMethod* callee : callees) {
        boost::optional<const DexType*> anno =
            type_inference::get_typedef_anno_from_member(
                callee, inference->get_annotations());
        if (anno == boost::none || anno != annotation) {
          DexType* return_type = callee->get_proto()->get_rtype();
          // constant folding might cause the source to be the invoked boolean
          // method https://fburl.com/code/h3dn0ft0
          if (type::is_boolean(return_type) && int_value_set->count(0) == 1 &&
              int_value_set->count(1) == 1) {
            break;
          }
          std::ostringstream out;
          out << "TypedefAnnoCheckerPass: the method "
              << show(def->get_method()->as_def())
              << "\n and any methods overriding it need to return a value with "
                 "the annotation "
              << show(annotation)
              << "\n and include it in it's method signature.\n"
              << " failed instruction: " << show(def) << "\n";
          m_good = false;
          m_error += out.str();
          return false;
        }
      }
      break;
    }
    case OPCODE_XOR_INT:
    case OPCODE_XOR_INT_LIT: {
      // https://fburl.com/code/7lk98pj6
      // in the code linked above, NotifLogAppBadgeEnabled.ENABLED has a value
      // of 0, and NotifLogAppBadgeEnabled.DISABLED_FROM_OS_ONLY has a value
      // of 1. We essentially end up with
      // mNotificationsSharedPrefsHelper.get().getAppBadgeEnabledStatus() ? 0 :
      // 1 which gets optimized to an XOR by the compiler
      if (int_value_set->count(0) == 0 || int_value_set->count(1) == 0) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the method" << show(m)
            << "\n assigns a int with typedef annotation " << show(annotation)
            << "\n to either 0 or 1, which is invalid because the typedef "
               "annotation class does not contain both the values 0 and 1.\n"
            << " failed instruction: " << show(def) << "\n";
        m_good = false;
        return false;
      }
      break;
    }
    case OPCODE_IGET:
    case OPCODE_SGET:
    case OPCODE_IGET_OBJECT:
    case OPCODE_SGET_OBJECT: {
      auto field_anno = type_inference::get_typedef_anno_from_member(
          def->get_field(), inference->get_annotations());
      if (!field_anno || field_anno != annotation) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << show(m)
            << "\n the field " << def->get_field()->str()
            << "\n needs to have the annotation " << show(annotation)
            << ".\n failed instruction: " << show(def) << "\n";
        m_error += out.str();
        m_good = false;
      }
      break;
    }
    default: {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: the method " << show(m)
          << "\n does not guarantee value safety for the value with typedef "
             "annotation "
          << show(annotation)
          << " .\n Check that this value does not change within the method\n"
          << " failed instruction: " << show(def) << "\n";
      m_good = false;
      m_error += out.str();
      return false;
    }
    }
  }
  return true;
}

void TypedefAnnoCheckerPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* unused */,
                                      PassManager& /* unused */) {
  assert(m_config.int_typedef != nullptr);
  assert(m_config.str_typedef != nullptr);
  auto scope = build_class_scope(stores);
  auto method_override_graph = mog::build_graph(scope);
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  SynthAccessorPatcher patcher(m_config, *method_override_graph);
  walk::parallel::classes(scope, [&](DexClass* cls) {
    gather_typedef_values(cls, strdef_constants, intdef_constants);

    // to reduce the number of walk::parallel::classes necessary,
    // run the first level nested lambda patcher here instead of
    // having a dedicated run along inside patcher.run
    if (klass::maybe_anonymous_class(cls)) {
      patcher.patch_first_level_nested_lambda(cls);
    }
  });

  patcher.run(scope);
  TRACE(TAC, 2, "Finish patching synth accessors");

  auto stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* m) {
    TypedefAnnoChecker checker = TypedefAnnoChecker(
        strdef_constants, intdef_constants, m_config, *method_override_graph);
    checker.run(m);
    if (!checker.complete()) {
      return Stats(checker.error());
    }
    return Stats();
  });

  if (stats.m_count > 0) {
    std::ostringstream out;
    out << "###################################################################"
           "\n"
        << "###################################################################"
           "\n"
        << "############ Typedef Annotation Value Safety Violation "
           "############\n"
        << "######### Please find the most recent diff that triggered "
           "#########\n"
        << "####### the error below and revert or add a fix to the diff "
           "#######\n"
        << "###################################################################"
           "\n"
        << "###################################################################"
           "\n"
        << "Encountered " << stats.m_count
        << " faulty methods. The errors are \n"
        << stats.m_errors << "\n";
    always_assert_log(false, "%s", out.str().c_str());
  }
}

void TypedefAnnoCheckerPass::gather_typedef_values(
    const DexClass* cls,
    StrDefConstants& strdef_constants,
    IntDefConstants& intdef_constants) {
  const std::vector<DexField*>& fields = cls->get_sfields();
  if (get_annotation(cls, m_config.str_typedef)) {
    std::unordered_set<const DexString*> str_values;
    for (auto* field : fields) {
      str_values.emplace(
          static_cast<DexEncodedValueString*>(field->get_static_value())
              ->string());
    }
    strdef_constants.emplace(cls, std::move(str_values));
  } else if (get_annotation(cls, m_config.int_typedef)) {
    std::unordered_set<uint64_t> int_values;
    for (auto* field : fields) {
      int_values.emplace(field->get_static_value()->value());
    }
    intdef_constants.emplace(cls, std::move(int_values));
  }
}

static TypedefAnnoCheckerPass s_pass;
