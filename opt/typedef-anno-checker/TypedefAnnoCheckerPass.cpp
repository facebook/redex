/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"

#include "AnnoUtils.h"
#include "ClassUtil.h"
#include "KotlinNullCheckMethods.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "TypedefAnnoPatcher.h"
#include "Walkers.h"

constexpr const char* DEFAULT_SUFFIX = "$default";

namespace {
bool is_null_check(const DexMethod* m) {
  for (DexMethodRef* kotlin_null_method :
       kotlin_nullcheck_wrapper::get_kotlin_null_assertions()) {
    if (kotlin_null_method->get_name() == m->get_name()) {
      return true;
    }
  }
  return m->get_deobfuscated_name_or_empty_copy() ==
         "Lcom/google/common/base/Preconditions;.checkNotNull:(Ljava/lang/"
         "Object;)Ljava/lang/Object;";
}

bool is_kotlin_result(const DexMethod* m) {
  return boost::starts_with(m->get_deobfuscated_name_or_empty_copy(),
                            "Lkotlin/Result");
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

// The patcher cannot annotate these read methods used by ModelGen, so the
// checker will fail when it expects an annotated returned value. We can skip
// these reads because all the ModelGen Parcel and Json write methods only write
// annotated values. Since all values that flow into the writes are annotated,
// we know that all reads are safe too.
bool is_parcel_or_json_read(const DexMethod* m) {
  return m->get_deobfuscated_name_or_empty_copy() ==
             "Landroid/os/Parcel;.readInt:()I" ||
         m->get_deobfuscated_name_or_empty_copy() ==
             "Landroid/os/Parcel;.readString:()Ljava/lang/String;" ||
         m->get_deobfuscated_name_or_empty_copy() ==
             "Lcom/facebook/common/json/"
             "AutoGenJsonHelper;.readStringValue:(Lcom/fasterxml/jackson/core/"
             "JsonParser;)Ljava/lang/String;" ||
         m->get_deobfuscated_name_or_empty_copy() ==
             "Lcom/fasterxml/jackson/core/JsonParser;.getValueAsInt:()I";
}

// The ModelGen class with the ModelDefinition annotation is the original
// _Spec class that all the other classes ($Builder, $Serializer, $Deserializer,
// etc.) are derived from. When we check a Parcel or Json read, we know that we
// are either in the $Builder or $Deserializer class and need to derive the base
// Spec class from the name
bool is_model_gen(const DexMethod* m) {
  DexType* type = m->get_class();
  auto model_gen_cls_name =
      type->str().substr(0, type->str().size() - 1) + "Spec;";
  DexClass* cls = type_class(DexType::make_type(model_gen_cls_name));
  if (!cls) {
    // the class could end in $Builder or $Deserializer
    auto model_gen_cls_name_from_builder =
        type->str().substr(0, type->str().find('$')) + "Spec;";
    cls = type_class(DexType::make_type(model_gen_cls_name_from_builder));
  }
  if (!cls || !cls->get_anno_set()) {
    return false;
  }
  DexAnnotation* anno = get_annotation(
      cls, DexType::make_type("Lcom/facebook/annotationprocessors/modelgen/"
                              "iface/ModelDefinition;"));
  if (anno) {
    return true;
  }
  return false;
}

bool is_composer_generated(const DexMethod* m) {
  DexType* type = m->get_class();
  DexClass* cls = type_class(type);
  if (!cls->get_anno_set()) {
    return false;
  }
  DexAnnotation* anno = get_annotation(
      cls, DexType::make_type(
               "Lcom/facebook/xapp/messaging/composer/annotation/Generated;"));
  if (anno) {
    return true;
  }
  return false;
}

} // namespace

bool TypedefAnnoChecker::is_value_of_opt(const DexMethod* m) {
  if (m->get_simple_deobfuscated_name() != "valueOfOpt") {
    return false;
  }

  // the util class
  auto cls = type_class(m->get_class());
  if (!boost::ends_with(cls->get_deobfuscated_name_or_empty_copy(), "$Util;")) {
    return false;
  }

  if (!cls || !cls->get_anno_set()) {
    return false;
  }

  DexClass* typedef_cls = nullptr;
  DexAnnotation* anno = get_annotation(
      cls, DexType::make_type("Ldalvik/annotation/EnclosingClass;"));
  if (anno) {
    auto& value = anno->anno_elems().begin()->encoded_value;
    if (value->evtype() == DexEncodedValueTypes::DEVT_TYPE) {
      auto type_value = static_cast<DexEncodedValueType*>(value.get());
      auto type_name = type_value->show_deobfuscated();
      typedef_cls = type_class(DexType::make_type(type_name));
    }
  }

  if (!typedef_cls || !typedef_cls->get_anno_set()) {
    return false;
  }

  if (!get_annotation(typedef_cls, m_config.int_typedef) &&
      !get_annotation(typedef_cls, m_config.str_typedef)) {
    return false;
  }
  return true;
}

bool TypedefAnnoChecker::is_delegate(const DexMethod* m) {
  auto* cls = type_class(m->get_class());
  DexTypeList* interfaces = cls->get_interfaces();

  if (interfaces->empty()) {
    return false;
  }

  auto& cfg = m->get_code()->cfg();
  DexField* delegate = nullptr;

  for (auto* block : cfg.blocks()) {
    for (const auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->opcode() == OPCODE_IGET_OBJECT) {
        DexField* field = insn->get_field()->as_def();
        if (!field) {
          continue;
        }
        // find methods that delegate with $$delegate_ P1697234372
        if (boost::starts_with(field->get_simple_deobfuscated_name(),
                               "$$delegate_")) {
          delegate = field;
        } else {
          // find methods that delegate without $$delegate_ P1698648093
          // the field type must match one of the interfaces
          for (auto interface : *interfaces) {
            if (interface->get_name() == field->get_type()->get_name()) {
              delegate = field;
            }
          }
        }
      } else if (opcode::is_an_invoke(insn->opcode()) && delegate) {
        auto* callee = insn->get_method()->as_def();
        if (!callee) {
          continue;
        }
        if (m->get_simple_deobfuscated_name() ==
                callee->get_simple_deobfuscated_name() &&
            m->get_proto() == callee->get_proto()) {
          TRACE(TAC, 2, "skipping delegate method %s", SHOW(m));
          return true;
        }
      }
    }
  }
  return false;
}

void TypedefAnnoChecker::run(DexMethod* m) {
  IRCode* code = m->get_code();
  if (!code) {
    return;
  }

  if (is_value_of_opt(m) || is_delegate(m)) {
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
  TRACE(TAC, 5, "Start checking %s", SHOW(m));
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
        } else if (typedef_anno::is_not_str_nor_int(env, reg)) {
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
    } else if (env_anno == boost::none && field_anno != boost::none) {
      bool good = check_typedef_value(m, field_anno, ud_chains, insn, 0,
                                      inference, envs);
      if (!good) {
        std::ostringstream out;
        out << " Error writing to field " << show(insn->get_field())
            << "in method" << SHOW(m) << "\n\n";
        m_error += out.str();
        TRACE(TAC, 1, "writing to field: %s", SHOW(insn->get_field()));
      }
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
      } else if (typedef_anno::is_not_str_nor_int(env, reg)) {
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
      if (const_value->str().empty() && is_composer_generated(m)) {
        break;
      }
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
      if (is_parcel_or_json_read(def_method) && is_model_gen(m)) {
        break;
      }
      // the result of usedef chains on a check cast could resolve to this
      // look further up for the real source
      if (is_null_check(def_method) || is_kotlin_result(def_method)) {
        check_typedef_value(m, annotation, ud_chains, def, 0, inference, envs);
        break;
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
    case OPCODE_NEW_INSTANCE: {
      live_range::MoveAwareChains chains(m->get_code()->cfg());
      live_range::DefUseChains du_chains = chains.get_def_use_chains();
      auto duchains_it = du_chains.find(def);
      auto uses_set = duchains_it->second;
      for (live_range::Use use : uses_set) {
        IRInstruction* use_insn = use.insn;
        if (opcode::is_an_iput(use_insn->opcode()) ||
            opcode::is_an_sput(use_insn->opcode())) {
          check_typedef_value(m, annotation, ud_chains, use_insn, 0, inference,
                              envs);
        }
      }
      break;
    }
    case OPCODE_CHECK_CAST: {
      check_typedef_value(m, annotation, ud_chains, def, 0, inference, envs);
      break;
    }
    case OPCODE_MOVE_EXCEPTION: {
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
                                      PassManager& mgr) {
  assert(m_config.int_typedef != nullptr);
  assert(m_config.str_typedef != nullptr);
  auto scope = build_class_scope(stores);
  auto method_override_graph = mog::build_graph(scope);
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoPatcher patcher(m_config, *method_override_graph);
  walk::parallel::classes(scope, [&](DexClass* cls) {
    gather_typedef_values(cls, strdef_constants, intdef_constants);
  });

  patcher.run(scope);

  mgr.set_metric("patched fields and methods",
                 patcher.get_patcher_stats().num_patched_fields_and_methods);
  mgr.set_metric("patched parameters",
                 patcher.get_patcher_stats().num_patched_parameters);

  mgr.set_metric(
      "patched chained fields and methods",
      patcher.get_chained_patcher_stats().num_patched_fields_and_methods);
  mgr.set_metric("patched chained parameters",
                 patcher.get_chained_patcher_stats().num_patched_parameters);

  mgr.set_metric("patched chained getter fields and methods",
                 patcher.get_chained_getter_patcher_stats()
                     .num_patched_fields_and_methods);
  mgr.set_metric(
      "patched chained getter parameters",
      patcher.get_chained_getter_patcher_stats().num_patched_parameters);
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
