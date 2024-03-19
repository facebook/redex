/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"

#include "AnnoUtils.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

constexpr const char* ACCESS_PREFIX = "access$";
constexpr const char* DEFAULT_SUFFIX = "$default";
constexpr const char* ANNOTATIONS_SUFFIX = "$annotations";
constexpr const char* COMPANION_CLASS = "$Companion";

namespace {

bool is_int(const type_inference::TypeEnvironment& env, reg_t reg) {
  return !env.get_int_type(reg).is_top() && !env.get_int_type(reg).is_bottom();
}

bool is_string(const type_inference::TypeEnvironment& env, reg_t reg) {
  return env.get_dex_type(reg) &&
         *env.get_dex_type(reg) == type::java_lang_String();
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

void collect_from_instruction(
    TypeEnvironments& envs,
    type_inference::TypeInference& inference,
    DexMethod* caller,
    IRInstruction* insn,
    std::vector<std::pair<int, DexAnnotationSet&>>& missing_param_annos) {
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
      if (anno_type && anno_type == annotation) {
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

} // namespace

void SynthAccessorPatcher::run(const Scope& scope) {
  walk::parallel::methods(scope, [this](DexMethod* m) {
    if (is_synthetic_accessor(m)) {
      collect_accessors(m);
    }
    if (is_synthetic_kotlin_annotations_method(m)) {
      patch_kotlin_annotations(m);
    }
  });
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
  // setters:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest$Companion.setField_one:(I)
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.access$setField_one$cp:(I)V
  // field is one of:
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.Field_one:I
  //    Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.field_one:I

  // some synthetic interfaces' names have $-CC. Delete it from the name
  auto class_name = m->get_class()->get_name()->str() + ".";
  auto pos = class_name.find("$-CC");
  if (pos != std::string::npos) class_name.erase(pos, 4);

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

  auto companion_pos = class_name.find(COMPANION_CLASS);
  if (companion_pos != std::string::npos) {
    class_name = class_name.substr(0, companion_pos) + ";.";
    // add annotations to access non-companion getter and setter methods
    add_annotations(
        DexMethod::get_method(class_name + ACCESS_PREFIX + "get" + field_name +
                              "$cp:()" + int_or_string),
        anno_set);
    add_annotations(
        DexMethod::get_method(class_name + ACCESS_PREFIX + "set" + field_name +
                              "$cp:(" + int_or_string + ")V"),
        anno_set);
  }
  // add annotations to field
  if (!add_annotations(
          DexField::get_field(class_name + field_name + ":" + int_or_string),
          anno_set)) {
    field_name.at(0) = std::tolower(field_name.at(0));
    add_annotations(
        DexField::get_field(class_name + field_name + ":" + int_or_string),
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
  std::vector<std::pair<int, DexAnnotationSet&>> missing_param_annos;
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      IROpcode opcode = insn->opcode();
      if (!opcode::is_an_invoke(opcode)) {
        continue;
      }
      collect_from_instruction(envs, inference, m, insn, missing_param_annos);
    }
  }

  // Patch missing param annotations
  for (auto& pair : missing_param_annos) {
    int param_index = pair.first;
    always_assert(is_static(m));
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
  TRACE(TAC, 2, "Start checking %s\n%s", SHOW(m), SHOW(cfg));
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
    type_inference::TypeInference* inference,
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
    type_inference::TypeInference* inference,
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
    case OPCODE_IGET_OBJECT: {
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
  walk::parallel::classes(scope, [&](DexClass* cls) {
    gather_typedef_values(cls, strdef_constants, intdef_constants);
  });

  SynthAccessorPatcher patcher(m_config, *method_override_graph);
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
