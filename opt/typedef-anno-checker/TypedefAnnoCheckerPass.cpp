/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"
#include "AnnoUtils.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

bool TypedefAnnoChecker::is_const(const type_inference::TypeEnvironment* env,
                                  reg_t reg) {
  return env->get_type(reg).element() == IRType::CONST ||
         env->get_type(reg).element() == IRType::ZERO;
}

bool TypedefAnnoChecker::is_string(const type_inference::TypeEnvironment* env,
                                   reg_t reg) {
  return env->get_dex_type(reg) &&
         env->get_dex_type(reg).value() == type::java_lang_String();
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
  type_inference::TypeInference inference(cfg, false, anno_set);
  inference.run(m);

  boost::optional<const DexType*> return_annotation = boost::none;
  auto return_annos = m->get_anno_set();
  if (return_annos) {
    return_annotation =
        inference.get_typedef_annotation(return_annos->get_annotations());
  }
  auto& envs = inference.get_type_environments();
  for (cfg::Block* b : cfg.blocks()) {
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      auto opcode = insn->opcode();
      auto& env = envs.find(insn)->second;

      check_instruction(m, &inference, env, insn, opcode, return_annotation);
    }
  }
}

void TypedefAnnoChecker::check_instruction(
    DexMethod* m,
    type_inference::TypeInference* inference,
    type_inference::TypeEnvironment& env,
    IRInstruction* insn,
    IROpcode opcode,
    boost::optional<const DexType*> return_annotation) {
  // if the invoked method's arguments have annotations with the
  // @SafeStringDef or @SafeIntDef annotation, check that TypeInference
  // inferred the correct annotation for the values being passed in
  switch (opcode) {
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    DexMethodRef* ref_method = insn->get_method();
    if (!ref_method->is_def()) {
      return;
    }
    DexMethod* def_method = ref_method->as_def();
    if (!def_method->get_param_anno()) {
      return;
    }
    for (auto const& param_anno : *def_method->get_param_anno()) {
      auto annotation = inference->get_typedef_annotation(
          param_anno.second->get_annotations());
      if (annotation == boost::none) {
        continue;
      }
      reg_t reg = insn->src(param_anno.first);
      auto env_anno = env.get_annotation(reg);

      // TypeInference inferred a different annotation
      if (env_anno != boost::none && env_anno != annotation) {
        std::ostringstream out;
        if (env_anno.value() == DexType::make_type("Ljava/lang/Object;")) {
          out << "TypedefAnnoCheckerPass: while invoking " << SHOW(def_method)
              << " in method " << SHOW(m) << ", parameter " << param_anno.first
              << "should have the annotation "
              << annotation.value()->get_name()->c_str()
              << " but it instead contains an ambiguous annotation, "
                 "implying that the parameter was joined with another "
                 "typedef annotation before the method invokation. The "
                 "ambiguous annotation is unsafe, and typedef annotations "
                 "should not be mixed.";
        } else {
          out << "TypedefAnnoCheckerPass: while invoking " << SHOW(def_method)
              << " in method " << SHOW(m) << ", parameter " << param_anno.first
              << " has the annotation " << SHOW(env_anno)
              << " , but the method expects the annotation to be "
              << annotation.value()->get_name()->c_str();
        }
        m_error = out.str();
        m_good = false;
      } else if (!is_const(&env, reg) && !is_string(&env, reg) &&
                 env.get_type(insn->src(0)) !=
                     type_inference::TypeDomain(IRType::INT)) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the annotation " << SHOW(annotation)
            << " annotates a parameter with an incompatible type or a "
               "non-constant parameter in method "
            << SHOW(m) << " while invoking method " << SHOW(def_method);
        m_error = out.str();
        m_good = false;
      }
    }
    break;
  }
  // when writing to annotated fields, check that the value is annotated
  case OPCODE_IPUT:
  case OPCODE_IPUT_OBJECT: {
    auto env_anno = env.get_annotation(insn->src(0));
    auto field_anno = inference->get_typedef_annotation(
        insn->get_field()->as_def()->get_anno_set()->get_annotations());
    if (env_anno != boost::none && field_anno != boost::none &&
        env_anno.value() != field_anno.value()) {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: The method " << SHOW(m)
          << " assigned a field " << insn->get_field()->c_str()
          << " with annotation " << SHOW(field_anno)
          << " to a value with annotation " << SHOW(env_anno);
      m_error = out.str();
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
              << " has an annotation "
              << return_annotation.value()->get_name()->c_str()
              << " in its method signature, but the returned value has an "
                 "ambiguous annotation, implying that the value was joined "
                 "with another typedef annotation within the method. The "
                 "ambiguous annotation is unsafe, and typedef annotations "
                 "should not be mixed.";
        } else {
          out << "TypedefAnnoCheckerPass: The method " << SHOW(m)
              << " has an annotation "
              << return_annotation.value()->get_name()->c_str()
              << " in its method signature, but the returned value "
                 "contains the annotation "
              << SHOW(env_anno) << " instead";
        }
        m_error = out.str();
        m_good = false;
      } else if (!is_const(&env, reg) && !is_string(&env, reg) &&
                 env.get_type(insn->src(0)) !=
                     type_inference::TypeDomain(IRType::INT)) {
        std::ostringstream out;
        out << "TypedefAnnoCheckerPass: the annotation "
            << SHOW(return_annotation)
            << " annotates a value with an incompatible type or a "
               "non-constant value in method "
            << SHOW(m);
        m_error = out.str();
        m_good = false;
      }
    }
    break;
  }
  default:
    break;
  }
}

void TypedefAnnoCheckerPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* unused */,
                                      PassManager& /* unused */) {
  assert(m_config.int_typedef != nullptr);
  assert(m_config.str_typedef != nullptr);
  auto scope = build_class_scope(stores);
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;

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
    out << "Encountered " << stats.m_count << " errors. The first error is \n"
        << stats.m_error << "\n";
    always_assert_log(false, "%s", out.str().c_str());
  }
}

static TypedefAnnoCheckerPass s_pass;
