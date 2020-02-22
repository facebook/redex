/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WholeProgramState.h"

#include "BaseIRAnalyzer.h"
#include "GlobalTypeAnalyzer.h"
#include "Resolver.h"
#include "Walkers.h"

using namespace type_analyzer;

namespace {

void set_sfields_in_partition(const DexClass* cls,
                              const DexTypeEnvironment& env,
                              DexTypeFieldPartition* field_partition) {
  // Note that we *must* iterate over the list of fields in the class and not
  // the bindings in env here. This ensures that fields whose values are unknown
  // (and therefore implicitly represented by Top in the env) get correctly
  // bound to Top in field_partition (which defaults its bindings to Bottom).
  for (auto& field : cls->get_sfields()) {
    auto value = env.get(field);
    if (!value.is_top()) {
      TRACE(TYPE, 2, "%s has value %s after <clinit> or <init>", SHOW(field),
            SHOW(value));
      always_assert(field->get_class() == cls->get_type());
    } else {
      TRACE(TYPE, 2, "%s has unknown value after <clinit> or <init>",
            SHOW(field));
    }
    field_partition->set(field, value);
  }
}

void analyze_clinits(const Scope& scope,
                     const global::GlobalTypeAnalyzer& gta,
                     DexTypeFieldPartition* field_partition) {
  for (DexClass* cls : scope) {
    auto clinit = cls->get_clinit();
    if (!clinit) {
      continue;
    }
    IRCode* code = clinit->get_code();
    auto& cfg = code->cfg();
    auto lta = gta.get_local_analysis(clinit);
    auto env = lta->get_exit_state_at(cfg.exit_block());
    set_sfields_in_partition(cls, env, field_partition);
  }
}

} // namespace

namespace type_analyzer {

WholeProgramState::WholeProgramState(
    const Scope& scope,
    const global::GlobalTypeAnalyzer& gta,
    const std::unordered_set<DexMethod*>& non_true_virtuals) {
  // TODO: Exclude fields that we cannot correctly infer their type?

  // Put non-root non true virtual methods in known methods.
  for (const auto& non_true_virtual : non_true_virtuals) {
    if (!root(non_true_virtual) && non_true_virtual->get_code()) {
      m_known_methods.emplace(non_true_virtual);
    }
  }
  walk::code(scope, [&](DexMethod* method, const IRCode&) {
    if (!method->is_virtual() && method->get_code()) {
      // Put non virtual methods in known methods.
      m_known_methods.emplace(method);
    }
  });

  analyze_clinits(scope, gta, &m_field_partition);
  collect(scope, gta);
}

void WholeProgramState::collect(const Scope& scope,
                                const global::GlobalTypeAnalyzer& gta) {
  ConcurrentMap<const DexField*, std::vector<DexTypeDomain>> fields_tmp;
  ConcurrentMap<const DexMethod*, std::vector<DexTypeDomain>> methods_tmp;
  walk::parallel::methods(scope, [&](DexMethod* method) {
    IRCode* code = method->get_code();
    if (code == nullptr) {
      return;
    }
    auto& cfg = code->cfg();
    auto lta = gta.get_local_analysis(method);
    for (cfg::Block* b : cfg.blocks()) {
      auto env = lta->get_entry_state_at(b);
      for (auto& mie : InstructionIterable(b)) {
        auto* insn = mie.insn;
        lta->analyze_instruction(insn, &env);
        collect_field_types(insn, env, &fields_tmp);
        collect_return_types(insn, env, method, &methods_tmp);
      }
    }
  });
  for (const auto& pair : fields_tmp) {
    for (auto& type : pair.second) {
      m_field_partition.update(pair.first, [&type](auto* current_type) {
        current_type->join_with(type);
      });
    }
  }
  for (const auto& pair : methods_tmp) {
    for (auto& type : pair.second) {
      m_method_partition.update(pair.first, [&type](auto* current_type) {
        current_type->join_with(type);
      });
    }
  }
}

void WholeProgramState::collect_field_types(
    const IRInstruction* insn,
    const DexTypeEnvironment& env,
    ConcurrentMap<const DexField*, std::vector<DexTypeDomain>>* field_tmp) {
  if (!is_sput(insn->opcode()) && !is_iput(insn->opcode())) {
    return;
  }
  auto field = resolve_field(insn->get_field());
  if (!field) {
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
    if (!method->get_proto()->is_void()) {
      // Reset RESULT_REGISTER, so the previous result would not get picked up
      // by the following move-result by accident.
      env->set(ir_analyzer::RESULT_REGISTER, type);
    }
    return false;
  }
  env->set(ir_analyzer::RESULT_REGISTER, type);
  return true;
}

} // namespace type_analyzer
