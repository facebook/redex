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
                       const UnorderedSet<TypedefAnnoType*>& typedef_annos) {
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
bool add_annotation_set(DexMember* member,
                        DexAnnotationSet* anno_set,
                        std::mutex& anno_patching_mutex,
                        Stats& class_stats) {
  if (member && member->is_def()) {
    auto def_member = member->as_def();
    std::lock_guard<std::mutex> lock(anno_patching_mutex);
    auto existing_annos = def_member->get_anno_set();
    if (existing_annos) {
      size_t anno_size = existing_annos->get_annotations().size();
      existing_annos->combine_with(*anno_set);
      if (existing_annos->get_annotations().size() != anno_size) {
        class_stats.num_patched_fields_and_methods += 1;
        TRACE(TAC, 3, "[patcher] patched member %s w/ %s", SHOW(member),
              SHOW(anno_set));
      }
    } else {
      DexAccessFlags access = def_member->get_access();
      def_member->set_access(ACC_SYNTHETIC);
      auto res = def_member->attach_annotation_set(
          std::make_unique<DexAnnotationSet>(*anno_set));
      always_assert(res);
      def_member->set_access(access);
      class_stats.num_patched_fields_and_methods += 1;
      TRACE(TAC, 3, "[patcher] patched member %s w/ %s", SHOW(member),
            SHOW(anno_set));
    }
    return true;
  }
  return false;
}

template <typename DexMember>
bool add_annotation(DexMember* member,
                    const TypedefAnnoType* anno,
                    std::mutex& anno_patching_mutex,
                    Stats& class_stats) {
  DexAnnotationSet anno_set = DexAnnotationSet();
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      const_cast<TypedefAnnoType*>(anno), DAV_RUNTIME));
  return add_annotation_set(member, &anno_set, anno_patching_mutex,
                            class_stats);
}

bool add_param_annotation_set(DexMethod* m,
                              DexAnnotationSet* anno_set,
                              const int param,
                              Stats& class_stats) {
  bool added = false;
  if (m->get_param_anno()) {
    if (m->get_param_anno()->count(param) == 1) {
      std::unique_ptr<DexAnnotationSet>& param_anno_set =
          m->get_param_anno()->at(param);
      if (param_anno_set != nullptr) {
        size_t anno_size = param_anno_set->get_annotations().size();
        param_anno_set->combine_with(*anno_set);
        if (param_anno_set->get_annotations().size() != anno_size) {
          class_stats.num_patched_parameters += 1;
          added = true;
        }
        return added;
      }
    }
  }
  DexAccessFlags access = m->get_access();
  m->set_access(ACC_SYNTHETIC);
  m->attach_param_annotation_set(param,
                                 std::make_unique<DexAnnotationSet>(*anno_set));
  m->set_access(access);
  return true;
}

bool add_param_annotation(DexMethod* m,
                          const TypedefAnnoType* anno,
                          const int param,
                          Stats& class_stats) {
  DexAnnotationSet anno_set = DexAnnotationSet();
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      const_cast<TypedefAnnoType*>(anno), DAV_RUNTIME));
  return add_param_annotation_set(m, &anno_set, param, class_stats);
}

void patch_param_candidates(std::vector<ParamCandidate>& param_candidates,
                            Stats& class_stats) {
  for (auto& param : param_candidates) {
    add_param_annotation(param.method, param.anno, param.index, class_stats);
  }
}

// Run usedefs to see if the source of the annotated value is a parameter
// If it is, return the index the parameter. If not, return -1
void find_and_patch_parameter(DexMethod* m,
                              IRInstruction* insn,
                              src_index_t arg_index,
                              const TypedefAnnoType* typedef_anno,
                              live_range::UseDefChains* ud_chains,
                              std::vector<ParamCandidate>& param_candidates) {
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
        param_candidates.emplace_back(m, typedef_anno, param_index);
        return;
      }
      param_index += 1;
    }
  }
}

void patch_param_from_method_invoke(
    TypeEnvironments& envs,
    type_inference::TypeInference& inference,
    DexMethod* caller,
    IRInstruction* invoke,
    live_range::UseDefChains* ud_chains,
    Stats& class_stats,
    std::vector<ParamCandidate>& missing_param_annos,
    bool patch_accessor = true) {
  always_assert(opcode::is_an_invoke(invoke->opcode()));
  auto* def_method = resolve_method(caller, invoke);
  if (!def_method ||
      (!def_method->get_param_anno() && !def_method->get_anno_set())) {
    // callee cannot be resolved, has no param annotation, or has no return
    // annotation
    return;
  }

  auto& invoke_env = envs.find(invoke)->second;
  if (def_method->get_param_anno() == nullptr) {
    return;
  }
  for (auto const& param_anno : *def_method->get_param_anno()) {
    auto param_typedef_anno = type_inference::get_typedef_annotation(
        param_anno.second->get_annotations(), inference.get_annotations());
    if (!param_typedef_anno) {
      continue;
    }
    int arg_index = invoke->opcode() == OPCODE_INVOKE_STATIC
                        ? param_anno.first
                        : param_anno.first + 1;
    reg_t arg_reg = invoke->src(arg_index);
    auto arg_anno = invoke_env.get_annotation(arg_reg);
    if (arg_anno && arg_anno == param_typedef_anno) {
      // Safe assignment. Nothing to do.
      continue;
    }
    find_and_patch_parameter(caller, invoke, arg_index, *param_typedef_anno,
                             ud_chains, missing_param_annos);
    TRACE(TAC, 2, "Missing param annotation %s in %s", SHOW(param_anno.second),
          SHOW(caller));
  }
}

/*
 * Collect missing param Typedef annotation if the current method writes to an
 * annotated field thru a value passed from param.
 */
void collect_setter_missing_param_annos(
    type_inference::TypeInference& inference,
    DexMethod* setter,
    IRInstruction* insn,
    live_range::UseDefChains* ud_chains,
    Stats& class_stats,
    std::vector<ParamCandidate>& missing_param_annos) {
  always_assert(opcode::is_an_iput(insn->opcode()) ||
                opcode::is_an_sput(insn->opcode()));
  auto field_ref = insn->get_field();
  auto field_anno = type_inference::get_typedef_anno_from_member(
      field_ref, inference.get_annotations());
  if (!field_anno) {
    return;
  }

  find_and_patch_parameter(setter, insn, 0, *field_anno, ud_chains,
                           missing_param_annos);
}

} // namespace

void PatchingCandidates::apply_patching(std::mutex& mutex, Stats& class_stats) {
  for (auto* field :
       unordered_to_ordered_keys(m_field_candidates, compare_dexfields)) {
    auto* anno = m_field_candidates.at(field);
    add_annotation(field, anno, mutex, class_stats);
  }
  for (auto* method :
       unordered_to_ordered_keys(m_method_candidates, compare_dexmethods)) {
    auto* anno = m_method_candidates.at(method);
    add_annotation(method, anno, mutex, class_stats);
  }
}

void TypedefAnnoPatcher::fix_kt_enum_ctor_param(const DexClass* cls,
                                                Stats& class_stats) {
  const auto& ctors = cls->get_ctors();
  for (auto* ctor : ctors) {
    if (is_synthetic(ctor)) {
      continue;
    }

    auto param_annos = ctor->get_param_anno();
    if (!param_annos) {
      continue;
    }
    const DexTypeList* args = ctor->get_proto()->get_args();
    for (const auto& panno : *param_annos) {
      auto annotation = type_inference::get_typedef_annotation(
          panno.second->get_annotations(), m_typedef_annos);
      if (annotation == boost::none) {
        continue;
      }
      size_t patch_idx = panno.first + 2;
      if (patch_idx >= args->size()) {
        continue;
      }
      if (!type::is_int(args->at(patch_idx)) &&
          args->at(patch_idx) != type::java_lang_String()) {
        continue;
      }
      if (add_param_annotation(ctor, *annotation, patch_idx, class_stats)) {
        TRACE(TAC, 2,
              "[patcher] Fixed Kotlin enum ctor param w/ %s at %ld on %s",
              SHOW(panno.second), patch_idx, SHOW(ctor));
      }
    }
  }
}

// https://kotlinlang.org/docs/fun-interfaces.html#sam-conversions
// sam conversions appear in Kotlin and provide a more concise way to override
// methods. This method handles sam conversiona and all synthetic methods that
// override methods with return or parameter annotations
bool TypedefAnnoPatcher::patch_if_overriding_annotated_methods(
    DexMethod* m, Stats& class_stats) {
  DexClass* cls = type_class(m->get_class());
  if (!klass::maybe_anonymous_class(cls)) {
    return false;
  }

  auto overriddens = mog::get_overridden_methods(m_method_override_graph, m,
                                                 true /*include_interfaces*/);
  for (auto overridden : UnorderedIterable(overriddens)) {
    auto return_anno = type_inference::get_typedef_anno_from_member(
        overridden, m_typedef_annos);

    if (return_anno != boost::none) {
      add_annotation(m, *return_anno, m_anno_patching_mutex, class_stats);
    }

    if (overridden->get_param_anno() == nullptr) {
      continue;
    }
    for (auto const& param_anno : *overridden->get_param_anno()) {
      auto annotation = type_inference::get_typedef_annotation(
          param_anno.second->get_annotations(), m_typedef_annos);
      if (annotation == boost::none) {
        continue;
      }
      add_param_annotation(m, *annotation, param_anno.first, class_stats);
    }
  }
  return false;
}

void TypedefAnnoPatcher::run(const Scope& scope) {
  m_patcher_stats =
      walk::parallel::classes<PatcherStats>(scope, [this](DexClass* cls) {
        // All the updates happening in this walk is local to the current class.
        // Therefore, there's no race condition.
        auto class_stats = PatcherStats();
        if (is_enum(cls) && type::is_kotlin_class(cls)) {
          fix_kt_enum_ctor_param(cls, class_stats.fix_kt_enum_ctor_param);
        }
        for (auto m : cls->get_all_methods()) {
          patch_parameters_and_returns(
              m, class_stats.patch_parameters_and_returns);
          patch_if_overriding_annotated_methods(
              m, class_stats.patch_synth_methods_overriding_annotated_methods);
          if (is_constructor(m) &&
              has_typedef_annos(m->get_param_anno(), m_typedef_annos)) {
            patch_synth_cls_fields_from_ctor_param(
                m, class_stats.patch_synth_cls_fields_from_ctor_param);
          }
        }
        return class_stats;
      });

  PatchingCandidates candidates;
  m_patcher_stats += walk::parallel::classes<PatcherStats>(
      scope, [this, &candidates](DexClass* cls) {
        auto class_stats = PatcherStats();

        if (!is_synthesized_lambda_class(cls) && !is_fun_interface_class(cls)) {
          return class_stats;
        }

        std::vector<const DexField*> patched_fields;
        for (auto m : cls->get_all_methods()) {
          patch_lambdas(m, &patched_fields, candidates,
                        class_stats.patch_lambdas);
        }
        if (!patched_fields.empty()) {
          auto cls_name = cls->get_deobfuscated_name_or_empty_copy();
          size_t after_first_dollar = cls_name.find('$') + 1;
          size_t class_prefix_end;
          if (isdigit(cls_name[after_first_dollar])) {
            // Patched lambda class is directly under the top most class. No
            // in-between chained lambda exists. We simply drop everything
            // after the 1st dollar. e.g.,
            // Lcom/xxx/yyy/zzz/TimelineCoverPhotoMenuBuilder$1
            class_prefix_end = after_first_dollar - 1;
          } else {
            // Patched lambda class is nested under an enclosing function.
            // There might be an enclosing chained lambda. We include whatever
            // is before the 2nd dollar. e.g.,
            // Lcom/xxx/yyy/zzz/TimelineCoverPhotoMenuBuilder$render
            class_prefix_end = cls_name.find('$', cls_name.find('$') + 1);
          }
          const auto class_prefix = cls_name.substr(0, class_prefix_end);
          const auto trace_update =
              [&patched_fields](const std::string& class_prefix) {
                std::ostringstream os;
                for (auto f : patched_fields) {
                  os << f->get_name()->str() << " " << f << "|";
                }
                TRACE(TAC, 2, "[patcher] Adding lambda map %s of fields %zu %s",
                      class_prefix.c_str(), patched_fields.size(),
                      os.str().c_str());
              };
          if (traceEnabled(TAC, 2)) {
            trace_update(class_prefix);
          }
          m_lambda_anno_map.update(class_prefix,
                                   [&patched_fields](auto, auto& fields, auto) {
                                     for (auto f : patched_fields) {
                                       fields.push_back(f);
                                     }
                                   });
        }
        return class_stats;
      });
  candidates.apply_patching(m_anno_patching_mutex,
                            m_patcher_stats.patch_lambdas);

  m_patcher_stats +=
      walk::parallel::classes<PatcherStats>(scope, [&](DexClass* cls) {
        auto class_stats = PatcherStats();
        if (klass::maybe_anonymous_class(cls) && get_enclosing_method(cls)) {
          patch_enclosing_lambda_fields(
              cls, class_stats.patch_enclosing_lambda_fields);
          patch_ctor_params_from_synth_cls_fields(
              cls, class_stats.patch_ctor_params_from_synth_cls_fields);
        }
        populate_chained_getters(cls);
        return class_stats;
      });

  patch_chained_getters(m_patcher_stats.patch_chained_getters);
}

void TypedefAnnoPatcher::populate_chained_getters(DexClass* cls) {
  auto class_name = cls->get_deobfuscated_name_or_empty_copy();
  auto class_name_prefix = class_name.substr(0, class_name.find('$'));
  if (m_patched_returns.count(class_name_prefix)) {
    m_chained_getters.insert(cls);
  }
}

void TypedefAnnoPatcher::patch_chained_getters(Stats& class_stats) {
  auto sorted_candidates =
      unordered_to_ordered(m_chained_getters, compare_dexclasses);
  for (auto* cls : sorted_candidates) {
    for (auto m : cls->get_all_methods()) {
      patch_parameters_and_returns(m, class_stats);
    }
  }
}

void TypedefAnnoPatcher::patch_ctor_params_from_synth_cls_fields(
    DexClass* cls, Stats& class_stats) {
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
        const auto& uses_set = udchains_it->second;
        for (live_range::Use use : UnorderedIterable(uses_set)) {
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
          add_param_annotation(ctor, *field_anno, param_idx - 2, class_stats);
        }
      }
    }
  }
}

void patch_synthetic_field_from_local_var_lambda(
    const live_range::UseDefChains& ud_chains,
    IRInstruction* insn,
    const src_index_t src,
    const TypedefAnnoType* anno,
    std::vector<const DexField*>* patched_fields,
    PatchingCandidates& candidates,
    std::mutex& anno_patching_mutex,
    Stats& class_stats) {
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
        candidates.add_field_candidate(original_field, anno);
      }
    } else {
      patched_fields->push_back(field);
      candidates.add_field_candidate(field, anno);
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
    PatchingCandidates& candidates,
    std::mutex& anno_patching_mutex,
    Stats& class_stats) {
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
      patch_synthetic_field_from_local_var_lambda(
          ud_chains, insn, param_anno.first + 1, *annotation, patched_fields,
          candidates, anno_patching_mutex, class_stats);
    }
  }
}

// Check all lambdas and function interfaces for any fields that need to be
// patched
void TypedefAnnoPatcher::patch_lambdas(
    DexMethod* method,
    std::vector<const DexField*>* patched_fields,
    PatchingCandidates& candidates,
    Stats& class_stats) {
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
        // the map of src index to annotation, but don't patch the static
        // method since a different visit of that method will patch it
        std::vector<ParamCandidate> missing_param_annos;
        patch_parameters_and_returns(static_method, class_stats,
                                     &missing_param_annos);
        // Patch missing param annotations
        for (auto& param : missing_param_annos) {
          patch_synthetic_field_from_local_var_lambda(
              ud_chains, insn, param.index, param.anno, patched_fields,
              candidates, m_anno_patching_mutex, class_stats);
        }
        // If the static method has parameter annotations, patch the synthetic
        // fields as expected
        if (static_method->get_param_anno()) {
          for (auto const& param_anno : *static_method->get_param_anno()) {
            auto annotation = type_inference::get_typedef_annotation(
                param_anno.second->get_annotations(), m_typedef_annos);
            if (annotation != boost::none) {
              patch_synthetic_field_from_local_var_lambda(
                  ud_chains, insn, param_anno.first, *annotation,
                  patched_fields, candidates, m_anno_patching_mutex,
                  class_stats);
            }
          }
        }
      } else if (opcode::is_invoke_interface(insn->opcode())) {
        auto* callee_def = resolve_method(method, insn);
        auto callees =
            mog::get_overriding_methods(m_method_override_graph, callee_def);
        for (auto callee : UnorderedIterable(callees)) {
          annotate_local_var_field_from_callee(
              callee, insn, ud_chains, inference, patched_fields, candidates,
              m_anno_patching_mutex, class_stats);
        }
      } else if (opcode::is_an_invoke(insn->opcode())) {
        auto* callee_def = resolve_method(method, insn);
        auto callees =
            mog::get_overriding_methods(m_method_override_graph, callee_def);
        callees.insert(callee_def);
        for (auto callee : UnorderedIterable(callees)) {
          annotate_local_var_field_from_callee(
              callee, insn, ud_chains, inference, patched_fields, candidates,
              m_anno_patching_mutex, class_stats);
        }
      }
    }
  }
}

// Given a constructor of a synthetic class, check if it has typedef annotated
// parameters. If it does, find the field that the parameter got put into and
// annotate it.
void TypedefAnnoPatcher::patch_synth_cls_fields_from_ctor_param(
    DexMethod* ctor, Stats& class_stats) {
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
        add_annotation(field, *annotation, m_anno_patching_mutex, class_stats);
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

void TypedefAnnoPatcher::patch_enclosing_lambda_fields(const DexClass* anon_cls,
                                                       Stats& class_stats) {
  auto anon_cls_name = anon_cls->get_deobfuscated_name_or_empty_copy();
  auto first_dollar = anon_cls_name.find('$');
  always_assert_log(first_dollar != std::string::npos,
                    "The enclosed method class %s should have a $ in the name",
                    SHOW(anon_cls));
  auto enclosing_cls_name = anon_cls_name.substr(0, first_dollar) + ";";
  DexClass* enclosing_class = type_class(
      DexType::make_type(DexString::make_string(enclosing_cls_name)));
  if (!enclosing_class) {
    return;
  }

  auto second_dollar = anon_cls_name.find('$', first_dollar + 1);
  // e.g.
  // Lcom/xxx/yyy/xxx/InspirationDiscImpl$maybeShowDisclosure
  auto enclosing_prefix = anon_cls_name.substr(0, second_dollar);
  auto it = m_lambda_anno_map.find(enclosing_prefix);
  if (it == m_lambda_anno_map.end() || it->second.empty()) {
    return;
  }
  auto patched_fields = it->second;
  TRACE(TAC, 2, "[patcher] lookup lambda map %s of field %zu",
        enclosing_prefix.c_str(), patched_fields.size());

  // Patch synthesized fields of the enclosing anonymous/lambda class. The field
  // being patched is on a class enclosing an inner lambda class with it's field
  // already patched in an earlier step.
  for (const auto patched_field : patched_fields) {
    auto enclosing_field_name = anon_cls_name + "." +
                                patched_field->get_simple_deobfuscated_name() +
                                ":" + patched_field->get_type()->str_copy();
    DexFieldRef* enclosing_field_ref =
        DexField::get_field(enclosing_field_name);
    if (enclosing_field_ref &&
        patched_field->get_deobfuscated_name() != enclosing_field_name) {
      DexField* enclosing_field = enclosing_field_ref->as_def();
      if (enclosing_field == nullptr) {
        continue;
      }
      auto typedef_anno = type_inference::get_typedef_anno_from_member(
          patched_field, m_typedef_annos);
      always_assert(typedef_anno != boost::none);
      TRACE(TAC, 2,
            "[patcher] patching enclosing field %s from patched field %s",
            SHOW(enclosing_field_name), SHOW(patched_field));
      add_annotation(enclosing_field, *typedef_anno, m_anno_patching_mutex,
                     class_stats);
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
    Stats& class_stats,
    std::vector<ParamCandidate>* missing_param_annos) {
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

  boost::optional<const TypedefAnnoType*> anno = boost::none;
  std::vector<ParamCandidate> param_candidates_local;
  bool patch_return = missing_param_annos == nullptr;
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (opcode::is_an_invoke(opcode)) {
        patch_param_from_method_invoke(envs, inference, m, insn, &ud_chains,
                                       class_stats, param_candidates_local);
      } else if (opcode::is_an_iput(opcode) || opcode::is_an_sput(opcode)) {
        collect_setter_missing_param_annos(inference, m, insn, &ud_chains,
                                           class_stats, param_candidates_local);
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

  if (missing_param_annos != nullptr) {
    missing_param_annos->insert(missing_param_annos->end(),
                                param_candidates_local.begin(),
                                param_candidates_local.end());
  } else {
    patch_param_candidates(param_candidates_local, class_stats);
  }

  if (patch_return && anno != boost::none) {
    add_annotation(m, const_cast<TypedefAnnoType*>(*anno),
                   m_anno_patching_mutex, class_stats);
    auto class_name = type_class(m->get_class())->str();
    auto class_name_prefix = class_name.substr(0, class_name.size() - 1);
    if (!m_patched_returns.count(class_name_prefix)) {
      m_patched_returns.insert(class_name_prefix);
    }
  }
}

void TypedefAnnoPatcher::print_stats(PassManager& mgr) {
  size_t total_member_patched = 0;
  size_t total_param_patched = 0;
  mgr.set_metric(
      "fix_kt_enum_ctor_param field/methods",
      m_patcher_stats.fix_kt_enum_ctor_param.num_patched_fields_and_methods);
  mgr.set_metric("fix_kt_enum_ctor_param params",
                 m_patcher_stats.fix_kt_enum_ctor_param.num_patched_parameters);
  TRACE(TAC, 1, "[patcher] fix_kt_enum_ctor_param field/methods %zu",
        m_patcher_stats.fix_kt_enum_ctor_param.num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] fix_kt_enum_ctor_param params %zu",
        m_patcher_stats.fix_kt_enum_ctor_param.num_patched_parameters);
  total_member_patched +=
      m_patcher_stats.fix_kt_enum_ctor_param.num_patched_fields_and_methods;
  total_param_patched +=
      m_patcher_stats.fix_kt_enum_ctor_param.num_patched_parameters;

  mgr.set_metric("patch_lambdas field/methods",
                 m_patcher_stats.patch_lambdas.num_patched_fields_and_methods);
  mgr.set_metric("patch_lambdas params",
                 m_patcher_stats.patch_lambdas.num_patched_parameters);
  TRACE(TAC, 1, "[patcher] patch_lambdas field/methods %zu",
        m_patcher_stats.patch_lambdas.num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] patch_lambdas params %zu",
        m_patcher_stats.patch_lambdas.num_patched_parameters);
  total_member_patched +=
      m_patcher_stats.patch_lambdas.num_patched_fields_and_methods;
  total_param_patched += m_patcher_stats.patch_lambdas.num_patched_parameters;

  mgr.set_metric("patch_parameters_and_returns field/methods",
                 m_patcher_stats.patch_parameters_and_returns
                     .num_patched_fields_and_methods);
  mgr.set_metric(
      "patch_parameters_and_returns params",
      m_patcher_stats.patch_parameters_and_returns.num_patched_parameters);
  TRACE(TAC, 1, "[patcher] patch_parameters_and_returns field/methods %zu",
        m_patcher_stats.patch_parameters_and_returns
            .num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] patch_parameters_and_returns params %zu",
        m_patcher_stats.patch_parameters_and_returns.num_patched_parameters);
  total_member_patched += m_patcher_stats.patch_parameters_and_returns
                              .num_patched_fields_and_methods;
  total_param_patched +=
      m_patcher_stats.patch_parameters_and_returns.num_patched_parameters;

  mgr.set_metric(
      "patch_synth_methods_overriding_annotated_methods field/methods",
      m_patcher_stats.patch_synth_methods_overriding_annotated_methods
          .num_patched_fields_and_methods);
  mgr.set_metric(
      "patch_synth_methods_overriding_annotated_methods params",
      m_patcher_stats.patch_synth_methods_overriding_annotated_methods
          .num_patched_parameters);
  TRACE(TAC, 1,
        "[patcher] patch_synth_methods_overriding_annotated_methods "
        "field/methods %zu",
        m_patcher_stats.patch_synth_methods_overriding_annotated_methods
            .num_patched_fields_and_methods);
  TRACE(TAC, 1,
        "[patcher] patch_synth_methods_overriding_annotated_methods params %zu",
        m_patcher_stats.patch_synth_methods_overriding_annotated_methods
            .num_patched_parameters);
  total_member_patched +=
      m_patcher_stats.patch_synth_methods_overriding_annotated_methods
          .num_patched_fields_and_methods;
  total_param_patched +=
      m_patcher_stats.patch_synth_methods_overriding_annotated_methods
          .num_patched_parameters;

  mgr.set_metric("patch_synth_cls_fields_from_ctor_param field/methods",
                 m_patcher_stats.patch_synth_cls_fields_from_ctor_param
                     .num_patched_fields_and_methods);
  mgr.set_metric("patch_synth_cls_fields_from_ctor_param params",
                 m_patcher_stats.patch_synth_cls_fields_from_ctor_param
                     .num_patched_parameters);
  TRACE(TAC, 1,
        "[patcher] patch_synth_cls_fields_from_ctor_param "
        "field/methods %zu",
        m_patcher_stats.patch_synth_cls_fields_from_ctor_param
            .num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] patch_synth_cls_fields_from_ctor_param params %zu",
        m_patcher_stats.patch_synth_cls_fields_from_ctor_param
            .num_patched_parameters);
  total_member_patched += m_patcher_stats.patch_synth_cls_fields_from_ctor_param
                              .num_patched_fields_and_methods;
  total_param_patched += m_patcher_stats.patch_synth_cls_fields_from_ctor_param
                             .num_patched_parameters;

  mgr.set_metric("patch_enclosing_lambda_fields field/methods",
                 m_patcher_stats.patch_enclosing_lambda_fields
                     .num_patched_fields_and_methods);
  mgr.set_metric(
      "patch_enclosing_lambda_fields params",
      m_patcher_stats.patch_enclosing_lambda_fields.num_patched_parameters);
  TRACE(TAC, 1,
        "[patcher] patch_enclosing_lambda_fields "
        "field/methods %zu",
        m_patcher_stats.patch_enclosing_lambda_fields
            .num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] patch_enclosing_lambda_fields params %zu",
        m_patcher_stats.patch_enclosing_lambda_fields.num_patched_parameters);
  total_member_patched += m_patcher_stats.patch_enclosing_lambda_fields
                              .num_patched_fields_and_methods;
  total_param_patched +=
      m_patcher_stats.patch_enclosing_lambda_fields.num_patched_parameters;

  mgr.set_metric("patch_ctor_params_from_synth_cls_fields field/methods",
                 m_patcher_stats.patch_ctor_params_from_synth_cls_fields
                     .num_patched_fields_and_methods);
  mgr.set_metric("patch_ctor_params_from_synth_cls_fields params",
                 m_patcher_stats.patch_ctor_params_from_synth_cls_fields
                     .num_patched_parameters);
  TRACE(TAC, 1,
        "[patcher] patch_ctor_params_from_synth_cls_fields "
        "field/methods %zu",
        m_patcher_stats.patch_ctor_params_from_synth_cls_fields
            .num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] patch_ctor_params_from_synth_cls_fields params %zu",
        m_patcher_stats.patch_ctor_params_from_synth_cls_fields
            .num_patched_parameters);
  total_member_patched +=
      m_patcher_stats.patch_ctor_params_from_synth_cls_fields
          .num_patched_fields_and_methods;
  total_param_patched += m_patcher_stats.patch_ctor_params_from_synth_cls_fields
                             .num_patched_parameters;

  mgr.set_metric(
      "patch_chained_getters field/methods",
      m_patcher_stats.patch_chained_getters.num_patched_fields_and_methods);
  mgr.set_metric("patch_chained_getters params",
                 m_patcher_stats.patch_chained_getters.num_patched_parameters);
  TRACE(TAC, 1,
        "[patcher] patch_chained_getters "
        "field/methods %zu",
        m_patcher_stats.patch_chained_getters.num_patched_fields_and_methods);
  TRACE(TAC, 1, "[patcher] patch_chained_getters params %zu",
        m_patcher_stats.patch_chained_getters.num_patched_parameters);
  total_member_patched +=
      m_patcher_stats.patch_chained_getters.num_patched_fields_and_methods;
  total_param_patched +=
      m_patcher_stats.patch_chained_getters.num_patched_parameters;

  mgr.set_metric("patched_returns", m_patched_returns.size());
  TRACE(TAC, 1, "[patcher] patched returns %zu", m_patched_returns.size());

  mgr.set_metric("total_member_patched", total_member_patched);
  TRACE(TAC, 1, "[patcher] total_member_patched %zu", total_member_patched);
  mgr.set_metric("total_param_patched", total_param_patched);
  TRACE(TAC, 1, "[patcher] total_param_patched %zu", total_param_patched);
}
