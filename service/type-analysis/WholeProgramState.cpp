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

std::ostream& operator<<(std::ostream& out, const DexField* field) {
  out << SHOW(field);
  return out;
}

std::ostream& operator<<(std::ostream& out, const DexMethod* method) {
  out << SHOW(method);
  return out;
}

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
      TRACE(TYPE, 5, "%s has value %s after <clinit>", SHOW(field),
            SHOW(value));
      always_assert(field->get_class() == cls->get_type());
    } else {
      TRACE(TYPE, 5, "%s has unknown value after <clinit>", SHOW(field));
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

bool returns_reference(const DexMethod* method) {
  auto rtype = method->get_proto()->get_rtype();
  return type::is_object(rtype);
}

} // namespace

namespace type_analyzer {

WholeProgramState::WholeProgramState(
    const Scope& scope,
    const global::GlobalTypeAnalyzer& gta,
    const std::unordered_set<DexMethod*>& non_true_virtuals) {
  // Exclude fields we cannot correctly analyze.
  walk::fields(scope, [&](DexField* field) {
    if (!type::is_object(field->get_type())) {
      return;
    }
    // We assume that a field we cannot delete is marked by a Proguard keep rule
    // or an annotation. The reason behind is that the field is referenced by
    // non-dex code.
    if (!can_delete(field)) {
      return;
    }
    m_known_fields.emplace(field);
  });

  // TODO: revisit this for multiple callee call graph.
  // Put non-root non true virtual methods in known methods.
  for (const auto& non_true_virtual : non_true_virtuals) {
    if (!root(non_true_virtual) && non_true_virtual->get_code() &&
        returns_reference(non_true_virtual)) {
      m_known_methods.emplace(non_true_virtual);
    }
  }
  walk::code(scope, [&](DexMethod* method, const IRCode&) {
    if (!method->is_virtual() && method->get_code() &&
        returns_reference(method)) {
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
    if (method::is_clinit(method)) {
      // No need to re-analyze clinits.
      return;
    }
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
  if (!field || !type::is_object(field->get_type())) {
    return;
  }
  auto type = env.get(insn->src(0));
  if (traceEnabled(TYPE, 5)) {
    std::ostringstream ss;
    ss << type;
    TRACE(TYPE, 5, "collecting field %s -> %s", SHOW(field), ss.str().c_str());
  }
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
  if (!returns_reference(method)) {
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

std::string WholeProgramState::print_field_partition_diff(
    const WholeProgramState& other) const {
  std::ostringstream ss;
  if (m_field_partition.is_top()) {
    ss << "[wps] diff this < is top" << std::endl;
    return ss.str();
  }
  if (other.m_field_partition.is_top()) {
    ss << "[wps] diff other > is top" << std::endl;
    return ss.str();
  }
  const auto& this_field_bindings = m_field_partition.bindings();
  const auto& other_field_bindings = other.m_field_partition.bindings();
  for (auto& pair : this_field_bindings) {
    auto field = pair.first;
    if (!other_field_bindings.count(field)) {
      ss << "[wps] diff " << field << " < " << pair.second << std::endl;
    } else {
      const auto& this_type = pair.second;
      const auto& other_type = other_field_bindings.at(field);
      if (!this_type.equals(other_type)) {
        ss << "[wps] diff " << field << " < " << this_type << " > "
           << other_type << std::endl;
      }
    }
  }
  for (auto& pair : other_field_bindings) {
    auto field = pair.first;
    if (!this_field_bindings.count(field)) {
      ss << "[wps] diff " << field << " > " << pair.second << std::endl;
    }
  }

  return ss.str();
}

std::string WholeProgramState::print_method_partition_diff(
    const WholeProgramState& other) const {
  std::ostringstream ss;
  if (m_method_partition.is_top()) {
    ss << "[wps] diff this < is top" << std::endl;
    return ss.str();
  }
  if (other.m_method_partition.is_top()) {
    ss << "[wps] diff other > is top" << std::endl;
    return ss.str();
  }
  const auto& this_method_bindings = m_method_partition.bindings();
  const auto& other_method_bindings = other.m_method_partition.bindings();
  for (auto& pair : this_method_bindings) {
    auto method = pair.first;
    if (!other_method_bindings.count(method)) {
      ss << "[wps] diff " << method << " < " << pair.second << std::endl;
    } else {
      const auto& this_type = pair.second;
      const auto& other_type = other_method_bindings.at(method);
      if (!this_type.equals(other_type)) {
        ss << "[wps] diff " << method << " < " << this_type << " > "
           << other_type << std::endl;
      }
    }
  }
  for (auto& pair : other_method_bindings) {
    auto method = pair.first;
    if (!this_method_bindings.count(method)) {
      ss << "[wps] diff " << method << " > " << pair.second << std::endl;
    }
  }

  return ss.str();
}

bool WholeProgramAwareAnalyzer::analyze_invoke(
    const WholeProgramState* whole_program_state,
    const IRInstruction* insn,
    DexTypeEnvironment* env) {
  if (whole_program_state == nullptr) {
    return false;
  }
  auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (method == nullptr) {
    return false;
  }
  if (!returns_reference(method)) {
    // Reset RESULT_REGISTER
    env->set(RESULT_REGISTER, DexTypeDomain::top());
    return false;
  }
  auto type = whole_program_state->get_return_type(method);
  env->set(RESULT_REGISTER, type);
  return true;
}

} // namespace type_analyzer
