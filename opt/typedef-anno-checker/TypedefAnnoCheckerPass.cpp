/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"

#include <boost/none.hpp>

#include "AnnoUtils.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

constexpr const char* ACCESS_PREFIX = "access$";
constexpr const char* DEFAULT_SUFFIX = "$default";

namespace {
bool is_int(const type_inference::TypeEnvironment* env, reg_t reg) {
  return env->get_type(reg).element() == IRType::INT ||
         env->get_type(reg).element() == IRType::CONST ||
         env->get_type(reg).element() == IRType::ZERO;
}

bool is_string(const type_inference::TypeEnvironment* env, reg_t reg) {
  return env->get_dex_type(reg) &&
         env->get_dex_type(reg).value() == type::java_lang_String();
}
} // namespace

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
  type_inference::TypeInference inference(cfg, false, anno_set);
  inference.run(m);

  live_range::MoveAwareChains chains(cfg);
  live_range::UseDefChains ud_chains = chains.get_use_def_chains();

  boost::optional<const DexType*> return_annotation = boost::none;
  DexAnnotationSet* return_annos = m->get_anno_set();
  if (return_annos) {
    return_annotation =
        inference.get_typedef_annotation(return_annos->get_annotations());
  }
  TypeEnvironments& envs = inference.get_type_environments();
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;

      check_instruction(m, &inference, insn, return_annotation, &ud_chains,
                        envs);
    }
  }
  if (!m_good) {
    TRACE(TAC, 1, "%s\n%s", SHOW(m), SHOW(cfg));
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
    auto def_method =
        resolve_method(insn->get_method(), opcode_to_search(insn), m);
    if (def_method == nullptr && opcode == OPCODE_INVOKE_VIRTUAL) {
      def_method =
          resolve_method(insn->get_method(), MethodSearch::InterfaceVirtual);
    }
    if (!def_method) {
      return;
    }
    // some methods are called through their access or defaul
    // counterpart, which do not retain the typedef annotation.
    // In these cases, we will skip the checker
    if (!def_method->get_param_anno() ||
        ACCESS_PREFIX + def_method->get_simple_deobfuscated_name() ==
            m->get_simple_deobfuscated_name() ||
        def_method->get_simple_deobfuscated_name() + DEFAULT_SUFFIX ==
            m->get_simple_deobfuscated_name()) {
      return;
    }
    for (auto const& param_anno : *def_method->get_param_anno()) {
      auto annotation = inference->get_typedef_annotation(
          param_anno.second->get_annotations());
      if (annotation == boost::none) {
        continue;
      }
      int param_index = insn->opcode() == OPCODE_INVOKE_STATIC
                            ? param_anno.first
                            : param_anno.first + 1;
      reg_t reg = insn->src(param_index);
      auto env_anno = env.get_annotation(reg);

      // TypeInference inferred a different annotation
      if (env_anno != boost::none && env_anno != annotation) {
        std::ostringstream out;
        if (env_anno.value() == DexType::make_type("Ljava/lang/Object;")) {
          out << "TypedefAnnoCheckerPass: while invoking " << SHOW(def_method)
              << "\n in method " << SHOW(m) << "\n parameter "
              << param_anno.first << "should have the annotation "
              << annotation.value()->get_name()->c_str()
              << "\n but it instead contains an ambiguous annotation, "
                 "implying that the parameter was joined with another "
                 "typedef annotation \n before the method invokation. The "
                 "ambiguous annotation is unsafe, and typedef annotations "
                 "should not be mixed.\n"
              << " failed instruction: " << SHOW(insn) << "\n\n";
        } else {
          out << "TypedefAnnoCheckerPass: while invoking " << SHOW(def_method)
              << "\n in method " << SHOW(m) << "\n parameter "
              << param_anno.first << " has the annotation " << SHOW(env_anno)
              << "\n but the method expects the annotation to be "
              << annotation.value()->get_name()->c_str()
              << ".\n failed instruction: " << SHOW(insn) << "\n\n";
        }
        m_error += out.str();
        m_good = false;
      } else if (!is_int(&env, reg) && !is_string(&env, reg) &&
                 env.get_type(insn->src(0)) !=
                     type_inference::TypeDomain(IRType::INT)) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the annotation " << SHOW(annotation)
            << "\n annotates a parameter with an incompatible type "
            << SHOW(env.get_type(reg))
            << "\n or a non-constant parameter in method " << SHOW(m)
            << "\n while trying to invoke the method " << SHOW(def_method)
            << ".\n failed instruction: " << SHOW(insn) << "\n\n";
        m_error += out.str();
        m_good = false;
      } else if (env_anno == boost::none) {
        // TypeInference didn't infer anything
        bool good = check_typedef_value(m, annotation, ud_chains, insn,
                                        param_index, inference, envs);
        if (!good) {
          std::ostringstream out;
          out << " Error invoking " << SHOW(def_method) << "\n";
          out << " Incorrect parameter's index: " << param_index << "\n\n";
          m_error += out.str();
          TRACE(TAC, 1, "invoke method: %s", SHOW(def_method));
        }
      }
    }
    break;
  }
  // when writing to annotated fields, check that the value is annotated
  case OPCODE_IPUT:
  case OPCODE_IPUT_OBJECT: {
    auto env_anno = env.get_annotation(insn->src(0));
    auto field_anno =
        inference->get_typedef_anno_from_member(insn->get_field());
    if (env_anno != boost::none && field_anno != boost::none &&
        env_anno.value() != field_anno.value()) {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: The method " << SHOW(m)
          << "\n assigned a field " << insn->get_field()->c_str()
          << "\n with annotation " << SHOW(field_anno)
          << "\n to a value with annotation " << SHOW(env_anno)
          << ".\n failed instruction: " << SHOW(insn) << "\n\n";
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
      auto env_anno = env.get_annotation(reg);
      if (env_anno != boost::none && env_anno != return_annotation) {
        std::ostringstream out;
        if (env_anno.value() == DexType::make_type("Ljava/lang/Object;")) {
          out << "TypedefAnnoCheckerPass: The method " << SHOW(m)
              << "\n has an annotation "
              << return_annotation.value()->get_name()->c_str()
              << "\n in its method signature, but the returned value has an "
                 "ambiguous annotation, implying that the value was joined \n"
                 "with another typedef annotation within the method. The "
                 "ambiguous annotation is unsafe, \nand typedef annotations "
                 "should not be mixed. \n"
              << "failed instruction: " << SHOW(insn) << "\n\n";
        } else {
          out << "TypedefAnnoCheckerPass: The method " << SHOW(m)
              << "\n has an annotation "
              << return_annotation.value()->get_name()->c_str()
              << "\n in its method signature, but the returned value "
                 "contains the annotation \n"
              << SHOW(env_anno) << " instead.\n"
              << " failed instruction: " << SHOW(insn) << "\n\n";
        }
        m_error += out.str();
        m_good = false;
      } else if (!is_int(&env, reg) && !is_string(&env, reg) &&
                 env.get_type(insn->src(0)) !=
                     type_inference::TypeDomain(IRType::INT)) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the annotation "
            << SHOW(return_annotation)
            << "\n annotates a value with an incompatible type or a "
               "non-constant value in method\n "
            << SHOW(m) << " .\n"
            << " failed instruction: " << SHOW(insn) << "\n\n";
        m_error += out.str();
        m_good = false;
      } else if (env_anno == boost::none) {
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
  live_range::Use use_of_id{insn, src};
  auto udchains_it = ud_chains->find(use_of_id);

  for (IRInstruction* def : udchains_it->second) {
    switch (def->opcode()) {
    case OPCODE_CONST_STRING: {
      auto const const_value = def->get_string();
      const auto& value_set = m_strdef_constants.at_unsafe(anno_class);
      if (value_set.count(const_value) == 0) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << SHOW(m)
            << "\n the string value " << SHOW(const_value)
            << " does not have the typedef annotation \n"
            << SHOW(annotation)
            << " attached to it. \n Check that the value is annotated and "
               "exists in the typedef annotation class.\n"
            << " failed instruction: " << SHOW(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
      }
      break;
    }
    case OPCODE_CONST: {
      auto const const_value = def->get_literal();
      const auto& value_set = m_intdef_constants.at_unsafe(anno_class);
      if (value_set.count(const_value) == 0) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << SHOW(m)
            << "\n the int value " << SHOW(const_value)
            << " does not have the typedef annotation \n"
            << SHOW(annotation)
            << " attached to it. \n Check that the value is annotated and "
               "exists in its typedef annotation class.\n"
            << " failed instruction: " << SHOW(def) << "\n";
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
        const auto& value_set = m_intdef_constants.at_unsafe(anno_class);
        if (value_set.count(0) == 0 || value_set.count(1) == 0) {
          std::ostringstream out;
          out << "TypedefAnnoCheckerPass: the method" << SHOW(m)
              << "\n assigns a int with typedef annotation " << SHOW(annotation)
              << "\n to either 0 or 1, which is invalid because the typedef "
                 "annotation class does not contain both the values 0 and 1.\n"
              << " failed instruction: " << SHOW(def) << "\n";
          m_good = false;
          return false;
        }
        break;
      }
      auto anno = env->second.get_annotation(def->dest());
      if (anno == boost::none || anno != annotation) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in method " << SHOW(m)
            << "\n one of the parameters needs to have the typedef annotation "
            << SHOW(annotation)
            << "\n attached to it. Check that the value is annotated and "
               "exists in the typedef annotation class.\n"
            << " failed instruction: " << SHOW(def) << "\n";
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
      auto def_method =
          resolve_method(def->get_method(), opcode_to_search(def), m);
      if (def_method == nullptr && def->opcode() == OPCODE_INVOKE_VIRTUAL) {
        def_method =
            resolve_method(def->get_method(), MethodSearch::InterfaceVirtual);
      }
      if (!def_method) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: in the method " << SHOW(m)
            << "\n the source of the value with annotation " << SHOW(annotation)
            << "\n is produced by invoking an unresolveable callee, so the "
               "value safety is not guaranteed.\n"
            << " failed instruction: " << SHOW(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
      }
      boost::optional<const DexType*> anno =
          inference->get_typedef_anno_from_member(def_method);
      if (anno == boost::none || anno != annotation) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the method "
            << SHOW(def->get_method()->as_def())
            << "\n needs to return a value with the anotation "
            << SHOW(annotation)
            << "\n and include it in it's method signature.\n"
            << " failed instruction: " << SHOW(def) << "\n";
        m_good = false;
        m_error += out.str();
        return false;
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
      const auto& value_set = m_intdef_constants.at_unsafe(anno_class);
      if (value_set.count(0) == 0 || value_set.count(1) == 0) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the method" << SHOW(m)
            << "\n assigns a int with typedef annotation " << SHOW(annotation)
            << "\n to either 0 or 1, which is invalid because the typedef "
               "annotation class does not contain both the values 0 and 1.\n"
            << " failed instruction: " << SHOW(def) << "\n";
        m_good = false;
        return false;
      }
      break;
    }
    default: {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: the method " << SHOW(m)
          << "\n does not guarantee value safety for the value with typedef "
             "annotation "
          << SHOW(annotation)
          << " .\n Check that this value does not change within the method\n"
          << " failed instruction: " << SHOW(def) << "\n";
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  walk::parallel::classes(scope, [&](DexClass* cls) {
    gather_typedef_values(cls, strdef_constants, intdef_constants);
  });

  auto stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* m) {
    TypedefAnnoChecker checker =
        TypedefAnnoChecker(strdef_constants, intdef_constants, m_config);
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
    ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>&
        strdef_constants,
    ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
        intdef_constants) {
  const std::vector<DexField*>& fields = cls->get_sfields();
  if (get_annotation(cls, m_config.str_typedef)) {
    std::unordered_set<const DexString*> str_values;
    for (auto* field : fields) {
      str_values.emplace(
          static_cast<DexEncodedValueString*>(field->get_static_value())
              ->string());
    }
    strdef_constants.insert(std::pair(cls, std::move(str_values)));
  } else if (get_annotation(cls, m_config.int_typedef)) {
    std::unordered_set<uint64_t> int_values;
    for (auto* field : fields) {
      int_values.emplace(field->get_static_value()->value());
    }
    intdef_constants.insert(std::pair(cls, std::move(int_values)));
  }
}

static TypedefAnnoCheckerPass s_pass;
