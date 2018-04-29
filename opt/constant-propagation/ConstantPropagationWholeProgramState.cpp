/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagationWholeProgramState.h"

#include "IPConstantPropagationAnalysis.h"
#include "Walkers.h"

using namespace constant_propagation;

namespace {

/*
 * Walk all the static fields in :cls, copying their bindings in :field_env over
 * to :field_partition.
 */
void set_fields_in_partition(const DexClass* cls,
                             const ConstantFieldEnvironment& field_env,
                             ConstantStaticFieldPartition* field_partition) {
  // Note that we *must* iterate over the list of fields in the class and not
  // the bindings in field_env here. This ensures that fields whose values are
  // unknown (and therefore implicitly represented by Top in the field_env)
  // get correctly bound to Top in field_partition (which defaults its
  // bindings to Bottom).
  for (auto& field : cls->get_sfields()) {
    auto value = field_env.get(field);
    if (!value.is_top()) {
      TRACE(ICONSTP, 2, "%s has value %s after <clinit>\n", SHOW(field),
            SHOW(value));
      always_assert(field->get_class() == cls->get_type());
    } else {
      TRACE(ICONSTP, 2, "%s has unknown value after <clinit>\n", SHOW(field));
    }
    field_partition->set(field, value);
  }
}

/*
 * Record in :field_partition the values of the static fields after the class
 * initializers have finished executing.
 *
 * XXX this assumes that there are no cycles in the class initialization graph!
 */
void analyze_clinits(const Scope& scope,
                     const interprocedural::FixpointIterator& fp_iter,
                     ConstantStaticFieldPartition* field_partition) {
  for (DexClass* cls : scope) {
    auto& dmethods = cls->get_dmethods();
    auto clinit_it = std::find_if(dmethods.begin(), dmethods.end(), is_clinit);
    if (clinit_it == dmethods.end()) {
      // If there is no class initializer, then the initial field values are
      // simply the DexEncodedValues.
      ConstantEnvironment env;
      set_encoded_values(cls, &env);
      set_fields_in_partition(cls, env.get_field_environment(),
                              field_partition);
      continue;
    }
    auto* clinit = *clinit_it;
    IRCode* code = clinit->get_code();
    auto& cfg = code->cfg();
    auto intra_cp = fp_iter.get_intraprocedural_analysis(clinit);
    auto env = intra_cp->get_exit_state_at(cfg.exit_block());
    set_fields_in_partition(cls, env.get_field_environment(), field_partition);
  }
}

} // namespace

namespace constant_propagation {

WholeProgramState::WholeProgramState(
    const Scope& scope, const interprocedural::FixpointIterator& fp_iter) {
  walk::fields(scope, [&](DexField* field) {
    // Currently, we only consider static fields in our analysis. We also
    // exclude those marked by keep rules: keep-marked fields may be written to
    // by non-Dex bytecode.
    // All fields not in m_known_fields will be bound to Top.
    if (is_static(field) && !root(field)) {
      m_known_fields.emplace(field);
    }
  });
  walk::code(scope, [&](DexMethod* method, const IRCode&) {
    m_known_methods.emplace(method);
  });
  analyze_clinits(scope, fp_iter, &m_field_partition);
  collect(scope, fp_iter);
}

/*
 * Walk over the entire program, doing a join over the values written to each
 * field, as well as a join over the values returned by each method.
 */
void WholeProgramState::collect(
    const Scope& scope, const interprocedural::FixpointIterator& fp_iter) {
  walk::methods(scope, [&](DexMethod* method) {
    IRCode* code = method->get_code();
    if (code == nullptr) {
      return;
    }
    auto& cfg = code->cfg();
    auto intra_cp = fp_iter.get_intraprocedural_analysis(method);
    for (cfg::Block* b : cfg.blocks()) {
      auto env = intra_cp->get_entry_state_at(b);
      for (auto& mie : InstructionIterable(b)) {
        auto* insn = mie.insn;
        intra_cp->analyze_instruction(insn, &env);
        collect_field_values(insn, env,
                             is_clinit(method) ? method->get_class() : nullptr);
        collect_return_values(insn, env, method);
      }
    }
  });
}

/*
 * For each static field, do a join over all the values that may have been
 * written to it at any point in the program.
 *
 * If we are encountering a field write of some value to Foo.someField in the
 * body of Foo.<clinit>, don't do anything -- that value will only be visible
 * to other methods if it remains unchanged up until the end of the <clinit>.
 * In that case, analyze_clinits() will record it.
 */
void WholeProgramState::collect_field_values(const IRInstruction* insn,
                                             const ConstantEnvironment& env,
                                             const DexType* clinit_cls) {
  if (!is_sput(insn->opcode())) {
    return;
  }
  auto field = resolve_field(insn->get_field());
  if (field != nullptr && m_known_fields.count(field)) {
    if (field->get_class() == clinit_cls) {
      return;
    }
    auto value = env.get(insn->src(0));
    m_field_partition.update(field, [&value](auto* current_value) {
      current_value->join_with(value);
    });
  }
}

/*
 * For each method, do a join over all the values that can be returned by it.
 *
 * If there are no reachable return opcodes in the method, then it never
 * returns. Its return value will be represented by Bottom in our analysis.
 */
void WholeProgramState::collect_return_values(const IRInstruction* insn,
                                              const ConstantEnvironment& env,
                                              const DexMethod* method) {
  auto op = insn->opcode();
  if (!is_return(op)) {
    return;
  }
  if (op == OPCODE_RETURN_VOID) {
    // We must set the binding to Top here to record the fact that this method
    // does indeed return -- even though `void` is not actually a return value,
    // this tells us that the code following any invoke of this method is
    // reachable.
    m_method_partition.update(
        method, [](auto* current_value) { current_value->set_to_top(); });
    return;
  }
  auto value = env.get(insn->src(0));
  m_method_partition.update(method, [&value](auto* current_value) {
    current_value->join_with(value);
  });
}

bool WholeProgramAwareSubAnalyzer::analyze_sget(
    const WholeProgramState* whole_program_state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  if (whole_program_state == nullptr) {
    return false;
  }
  auto field = resolve_field(insn->get_field());
  if (field == nullptr) {
    return false;
  }
  auto value = whole_program_state->get_field_value(field);
  if (value.is_top()) {
    return false;
  }
  env->set(RESULT_REGISTER, value);
  return true;
}

bool WholeProgramAwareSubAnalyzer::analyze_invoke(
    const WholeProgramState* whole_program_state,
    const IRInstruction* insn,
    ConstantEnvironment* env) {
  if (whole_program_state == nullptr) {
    return false;
  }
  auto op = insn->opcode();
  if (op != OPCODE_INVOKE_DIRECT && op != OPCODE_INVOKE_STATIC) {
    return false;
  }
  auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (method == nullptr) {
    return false;
  }
  auto value = whole_program_state->get_return_value(method);
  if (value.is_top()) {
    return false;
  }
  env->set(RESULT_REGISTER, value);
  return true;
}

} // namespace constant_propagation
