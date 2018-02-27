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

namespace constant_propagation {

WholeProgramState::WholeProgramState(
    const Scope& scope, const interprocedural::FixpointIterator& fp_iter) {
  walk::fields(scope, [&](DexField* field) {
    // Currently, we only consider static methods in our analysis. All other
    // fields values will be represented by Top.
    if (is_static(field)) {
      m_known_fields.emplace(field);
    }
  });
  walk::code(scope, [&](DexMethod* method, const IRCode&) {
    m_known_methods.emplace(method);
  });
  set_fields_with_encoded_values(scope);
  collect(scope, fp_iter);
}

/*
 * Initialize m_field_partition with the encoded values in the dex. If no
 * encoded value is present, initialize them with a zero value (which is the
 * same thing that the runtime does).
 */
void WholeProgramState::set_fields_with_encoded_values(const Scope& scope) {
  for (const auto* cls : scope) {
    for (auto* sfield : cls->get_sfields()) {
      auto value = sfield->get_static_value();
      if (value == nullptr) {
        m_field_partition.set(sfield, SignedConstantDomain(0));
      } else if (is_primitive(sfield->get_type())) {
        m_field_partition.set(sfield, SignedConstantDomain(value->value()));
      } else {
        // XXX this is probably overly conservative. A field that isn't
        // primitive may still have an explicitly-encoded zero value. However,
        // note that we can't simply use DexEncodedValue::value() to check
        // for a zero value -- for non-primitive encoded values, that method
        // always returns zero! We need to fix that API...
        m_field_partition.set(sfield, SignedConstantDomain::top());
      }
    }
  }
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
    for (Block* b : cfg.blocks()) {
      auto env = intra_cp->get_entry_state_at(b);
      for (auto& mie : InstructionIterable(b)) {
        auto* insn = mie.insn;
        intra_cp->analyze_instruction(insn, &env);
        collect_field_values(insn, env);
        collect_return_values(insn, env, method);
      }
    }
  });
}

/*
 * For each static field, do a join over all the values that may have been
 * written to it at any point in the program.
 */
void WholeProgramState::collect_field_values(const IRInstruction* insn,
                                             const ConstantEnvironment& env) {
  if (!is_sput(insn->opcode())) {
    return;
  }
  auto value = env.get(insn->src(0));
  auto field = resolve_field(insn->get_field());
  if (field != nullptr && m_known_fields.count(field)) {
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

} // namespace constant_propagation
