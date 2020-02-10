/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WholeProgramState.h"

#include "BaseIRAnalyzer.h"
#include "Resolver.h"
#include "Walkers.h"

using namespace type_analyzer;

namespace {

bool analyze_gets_helper(const WholeProgramState* whole_program_state,
                         const IRInstruction* insn,
                         DexTypeEnvironment* env) {
  if (whole_program_state == nullptr) {
    return false;
  }
  auto field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  auto type = whole_program_state->get_field_type(field);
  if (type.is_top()) {
    return false;
  }
  env->set(ir_analyzer::RESULT_REGISTER, type);
  return true;
}

} // namespace

namespace type_analyzer {

WholeProgramState::WholeProgramState(const Scope& scope,
                                     const global::GlobalTypeAnalyzer&) {
  walk::fields(scope, [&](DexField* field) { m_known_fields.emplace(field); });

  walk::code(scope, [&](DexMethod* method, const IRCode&) {
    if (method->get_code()) {
      // TODO: Consider if we should add constraints for known methods and
      // fields.
      m_known_methods.emplace(method);
    }
  });

  // TODO: collect types.
}

void WholeProgramState::collect_field_types(
    const IRInstruction* insn,
    const DexTypeEnvironment& env,
    const DexType* clinit_cls,
    ConcurrentMap<const DexField*, std::vector<DexTypeDomain>>* field_tmp) {
  if (!is_sput(insn->opcode()) && !is_iput(insn->opcode())) {
    return;
  }
  auto field = resolve_field(insn->get_field());
  if (!field || !m_known_fields.count(field)) {
    return;
  }
  if (is_sput(insn->opcode()) && field->get_class() == clinit_cls) {
    return;
  }
  auto type = env.get(insn->src(0));
  field_tmp->update(field,
                    [type](const DexField*,
                           std::vector<DexTypeDomain>& s,
                           bool /* exists */) { s.emplace_back(type); });
}

void WholeProgramState::collect_return_types(
    const IRInstruction* insn,
    const DexTypeEnvironment& env,
    const DexMethod* method,
    ConcurrentMap<const DexMethod*, std::vector<DexTypeDomain>>* method_tmp) {
  auto op = insn->opcode();
  if (!is_return(op)) {
    return;
  }
  if (op == OPCODE_RETURN_VOID) {
    // We must set the binding to Top here to record the fact that this method
    // does indeed return -- even though `void` is not actually a return type,
    // this tells us that the code following any invoke of this method is
    // reachable.
    method_tmp->update(
        method,
        [](const DexMethod*, std::vector<DexTypeDomain>& s, bool /* exists */) {
          s.emplace_back(DexTypeDomain::top());
        });
    return;
  }
  auto type = env.get(insn->src(0));
  method_tmp->update(method,
                     [type](const DexMethod*,
                            std::vector<DexTypeDomain>& s,
                            bool /* exists */) { s.emplace_back(type); });
}

bool WholeProgramAwareAnalyzer::analyze_sget(
    const WholeProgramState* whole_program_state,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  return analyze_gets_helper(whole_program_state, insn, env);
}

bool WholeProgramAwareAnalyzer::analyze_iget(
    const WholeProgramState* whole_program_state,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  return analyze_gets_helper(whole_program_state, insn, env);
}

bool WholeProgramAwareAnalyzer::analyze_invoke(
    const WholeProgramState* whole_program_state,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  if (whole_program_state == nullptr) {
    return false;
  }
  auto op = insn->opcode();
  if (op != OPCODE_INVOKE_DIRECT && op != OPCODE_INVOKE_STATIC &&
      op != OPCODE_INVOKE_VIRTUAL) {
    return false;
  }
  auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (method == nullptr) {
    return false;
  }
  auto type = whole_program_state->get_return_type(method);
  if (type.is_top()) {
    return false;
  }
  env->set(ir_analyzer::RESULT_REGISTER, type);
  return true;
}

} // namespace type_analyzer
