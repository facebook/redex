/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypedefAnnoCheckerPass.h"

#include <boost/none.hpp>

#include "AnnoUtils.h"
#include "FinalInlineV2.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Walkers.h"

using CombinedInitAnalyzer =
    InstructionAnalyzerCombiner<constant_propagation::StringAnalyzer,
                                constant_propagation::PrimitiveAnalyzer>;

namespace {
bool is_const(const type_inference::TypeEnvironment* env, reg_t reg) {
  return env->get_type(reg).element() == IRType::CONST ||
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

  ConstantEnvironment const_env;
  constant_propagation::intraprocedural::FixpointIterator intra_cp(
      cfg, CombinedInitAnalyzer(nullptr, nullptr));
  intra_cp.run(const_env);

  boost::optional<const DexType*> return_annotation = boost::none;
  DexAnnotationSet* return_annos = m->get_anno_set();
  if (return_annos) {
    return_annotation =
        inference.get_typedef_annotation(return_annos->get_annotations());
  }
  auto& envs = inference.get_type_environments();
  for (cfg::Block* b : cfg.blocks()) {
    const_env = intra_cp.get_exit_state_at(b);
    for (auto& mie : InstructionIterable(b)) {
      auto* insn = mie.insn;
      auto opcode = insn->opcode();
      auto& env = envs.find(insn)->second;

      check_instruction(m, &inference, env, insn, &opcode, return_annotation,
                        const_env);
    }
  }
}

void TypedefAnnoChecker::check_instruction(
    DexMethod* m,
    type_inference::TypeInference* inference,
    type_inference::TypeEnvironment& env,
    IRInstruction* insn,
    IROpcode* opcode,
    const boost::optional<const DexType*>& return_annotation,
    const ConstantEnvironment& const_env) {
  // if the invoked method's arguments have annotations with the
  // @SafeStringDef or @SafeIntDef annotation, check that TypeInference
  // inferred the correct annotation for the values being passed in
  switch (*opcode) {
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
      } else if (env_anno == boost::none) {
        // TypeInference didn't infer anything
        check_typedef_value(m, &env, annotation, reg, const_env);
        if (!m_good) {
          std::ostringstream out;
          out << " This error occured while invoking the method "
              << SHOW(def_method);
          m_error += out.str();
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
      } else if (env_anno == boost::none) {
        check_typedef_value(m, &env, return_annotation, reg, const_env);
      }
    }
    break;
  }
  default:
    break;
  }
}

void TypedefAnnoChecker::check_typedef_value(
    DexMethod* m,
    const type_inference::TypeEnvironment* env,
    const boost::optional<const DexType*>& annotation,
    reg_t reg,
    const ConstantEnvironment& const_env) {

  auto anno_class = type_class(annotation.value());
  // Throw an error if the IRType is CONST or ZERO and the value
  // is not in m_intdef_constants
  if (is_const(env, reg)) {
    const auto value_set = m_intdef_constants.at_unsafe(anno_class);
    auto const_domain = const_env.get<SignedConstantDomain>(reg);

    auto const_value = const_domain.get_constant();
    if (value_set.count(const_value.value()) == 0) {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: the value " << const_value.value()
          << " in method " << SHOW(m)
          << " either does not have the typedef annotation " << SHOW(annotation)
          << " or was not declared as one of the possible values in the "
             "annotation.";
      m_error = out.str();
      m_good = false;
    }
    return;
  }
  // Throw an error if the DexType is String and the value is
  // not in m_strdef_constants
  else if (is_string(env, reg)) {
    const auto& value_set = m_strdef_constants.at_unsafe(anno_class);
    auto const_domain = const_env.get<StringDomain>(reg);

    auto const_value = const_domain.get_constant();
    // Throw an error if the string is not a constant
    if (const_value == boost::none) {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: the method " << SHOW(m)
          << " changes a value with annotation " << SHOW(annotation)
          << " midway, which is unsafe.";
      m_error = out.str();
      m_good = false;
    } else if (value_set.count(const_value.value()->str_copy()) == 0) {
      std::ostringstream out;
      out << "TypedefAnnoCheckerPass: the value "
          << const_value.value()->c_str() << " in method " << SHOW(m)
          << " either does not have the typedef annotation " << SHOW(annotation)
          << " or was not declared as one of the possible values in the "
             "annotation.";
      m_error = out.str();
      m_good = false;
    }
    return;
  }
  // Throw an error because the annotation is attached to a non-const int
  else {
    std::ostringstream out;
    out << "TypedefAnnoCheckerPass: the annotation " << SHOW(annotation)
        << " annotates a non-constant integral in method " << SHOW(m)
        << " . Check that the type and annotation in the method signature are "
           "valid and the value has not changed throughout the method.";
    m_error = out.str();
    m_good = false;
  }
}

void TypedefAnnoCheckerPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* unused */,
                                      PassManager& /* unused */) {
  assert(m_config.int_typedef != nullptr);
  assert(m_config.str_typedef != nullptr);
  auto scope = build_class_scope(stores);
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
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
    out << "Encountered " << stats.m_count << " errors. The first error is \n"
        << stats.m_error << "\n";
    always_assert_log(false, "%s", out.str().c_str());
  }
}

void TypedefAnnoCheckerPass::gather_typedef_values(
    const DexClass* cls,
    ConcurrentMap<const DexClass*, std::unordered_set<std::string>>&
        strdef_constants,
    ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
        intdef_constants) {
  const std::vector<DexField*>& fields = cls->get_sfields();
  if (get_annotation(cls, m_config.str_typedef)) {
    std::unordered_set<std::string> str_values;
    for (auto* field : fields) {
      str_values.emplace(
          static_cast<DexEncodedValueString*>(field->get_static_value())
              ->string()
              ->c_str());
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
