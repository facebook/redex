/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassInitCounter.h"

#include <utility>
#include <vector>

#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "IRCode.h"
#include "ScopedCFG.h"
#include "Show.h"

using namespace cic;

bool operator==(const FieldSet& a, const FieldSet& b) {
  if (a.set != b.set || a.source != b.source) {
    return false;
  }
  std::unordered_set<IRInstruction*> a_instrs;
  std::unordered_set<IRInstruction*> b_instrs;
  for (const auto& a_reg_instr : a.regs) {
    a_instrs.insert(a_reg_instr.second.begin(), a_reg_instr.second.end());
  }
  for (const auto& b_reg_instr : b.regs) {
    b_instrs.insert(b_reg_instr.second.begin(), b_reg_instr.second.end());
  }
  return a_instrs == b_instrs;
}

inline bool operator!=(const FieldSet& a, const FieldSet& b) {
  return !(a == b);
}

inline bool operator==(const MethodCall& a, const MethodCall& b) {
  if (a.call != b.call) {
    return false;
  }
  std::unordered_set<IRInstruction*> a_instrs;
  std::unordered_set<IRInstruction*> b_instrs;
  for (const auto& call : a.call_sites) {
    a_instrs.insert(call.first);
  }
  for (const auto& call : b.call_sites) {
    b_instrs.insert(call.first);
  }
  return a_instrs == b_instrs;
}

inline bool operator!=(const MethodCall& a, const MethodCall& b) {
  return !(a == b);
}

std::string show(FlowStatus f) {
  switch (f) {
  case Conditional:
    return "Conditional";
  case AllPaths:
    return "AllPaths";
  }
  return "NotFlow";
}

std::string show(SourceStatus s) {
  switch (s) {
  case OneReg:
    return "OneReg";
  case MultipleReg:
    return "MultipleReg";
  case Unclear:
    return "Unclear";
  }
  return "NotSource";
}

std::string show(const FieldSetMap& fields_writes) {
  std::stringstream out;
  out << "[";
  for (const auto& field_set : fields_writes) {
    out << "{\"field_set\" : \"" << field_set.first->get_name()->str()
        << "\", \"num_src_registers : " << field_set.second.regs.size()
        << ", \"source\" : \"" << show(field_set.second.source)
        << "\", \"flow\" : \"" << show(field_set.second.set) << "\"},";
  }
  out << "]";
  return out.str();
}

std::string show(const FieldReads& field_reads) {
  std::stringstream out;
  out << "[";
  for (const auto& field_read : field_reads.get_fields()) {
    out << "{\"field_read\" : \"" << field_read.first->get_name()->str()
        << "\"flow\" :\"" << show(field_read.second) << "\"},";
  }
  out << "]";
  return out.str();
}

std::string show(const CallMap& methods) {
  std::stringstream out;
  out << "[";
  for (const auto& method_call : methods) {
    out << "{\"method\" : \"" << method_call.first->get_name()->str()
        << "\", \"call_count\" : " << method_call.second.call_sites.size()
        << "},";
  }
  out << "]";
  return out.str();
}

std::string show(const Escapes& escapes) {
  std::stringstream out;
  out << "{\"Escape_method\" : {"
      << "\"via_return\" : \""
      << (escapes.via_return ? show(escapes.via_return.value())
                             : "\"NoReturn\"")
      << "\", "
      << "\"via_array_write\" : "
      << (escapes.via_array_write.empty() ? "\"none\"," : "[");
  for (const auto& i_flow : escapes.via_array_write) {
    out << "\"" << show(i_flow.second) << "\",";
  }
  out << (escapes.via_array_write.empty() ? "" : "],")
      << "\"via_field\" : " << show(escapes.via_field_set) << ", "
      << "\"via_static_method\" : " << show(escapes.via_smethod_call) << ", "
      << "\"via_virtual_method\" : " << show(escapes.via_vmethod_call) << "}}";
  return out.str();
}

std::string show(const ObjectUses& object_uses) {
  std::stringstream out;
  out << "{\"UsageData : {"
      << "\"created_flow\" : \"" << show(object_uses.created_flow) << "\", "
      << "\"field_reads\" : " << show(object_uses.fields_read) << ", "
      << "\"field_writes\" : " << show(object_uses.fields_set.get_fields())
      << ", "
      << "\"method_calls\" : " << show(object_uses.method_calls.get_calls())
      << ", "
      << "\"escapes\" : " << show(object_uses.escapes) << ", "
      << "\"safe-escapes\" : " << show(object_uses.safe_escapes) << "}}";
  return out.str();
}

std::string show(const InitLocation& init) {
  std::stringstream out;
  out << "{\"Init\" : { \"type\" : " << SHOW(init.m_typ)
      << ", \"count\" : " << init.get_count() << ", \"data\" : [";
  for (const auto& class_inits : init.get_inits()) {
    for (const auto& method_inits : class_inits.second) {
      for (const auto& instr_inits : method_inits.second) {
        for (const auto& use : instr_inits.second) {
          out << "{\"class\" : \"" << class_inits.first->get_name()->c_str()
              << "\", "
              << "\"method\" : \"" << method_inits.first->get_name()->c_str()
              << "\", "
              << "\"instr\" : \"" << show(instr_inits.first) << "\", "
              << "\"usage\" : " << show(*use) << "}, ";
        }
      }
    }
  }
  out << "]}";
  return out.str();
}

bool field_subset_eq(const FieldSetMap& base, FieldSetMap other) {
  for (const auto& field : base) {
    auto other_field = other.find(field.first);
    if (other_field == other.end()) {
      return false;
    }
    if (other_field->second != field.second) {
      return false;
    }
  }
  return true;
}

bool calls_subset_eq(const CallMap& base, CallMap other) {
  for (const auto& call : base) {
    auto other_call = other.find(call.first);
    if (other_call == other.end()) {
      return false;
    }
    if (other_call->second != call.second) {
      return false;
    }
  }
  return true;
}

// Assumes two OneReg's compare registers externally
SourceStatus path_combine(SourceStatus path_a, SourceStatus path_b) {
  if (path_a == path_b) {
    return path_a;
  }
  if (path_a == OneReg) {
    return path_b;
  }
  if (path_b == OneReg) {
    return path_a;
  }
  return Unclear;
}

FlowStatus path_combine(FlowStatus path_a, FlowStatus path_b) {
  if (path_a == path_b) {
    return path_a;
  }
  return Conditional;
}

SourceStatus path_merge(SourceStatus path_a, SourceStatus path_b) {
  if (path_a == path_b) {
    return path_a;
  }
  // Assumes two OneReg's compare registers externally
  if (path_a == OneReg) {
    return path_b;
  }
  if (path_b == OneReg) {
    return path_a;
  }
  return path_b;
}

// Look for inconsistencies but not having all the same fields is allowed
// from different paths since we do not model field reads based on seen writes
bool FieldWriteRegs::consistent_with(const FieldWriteRegs& other) {
  return field_subset_eq(m_fields, other.m_fields) ||
         field_subset_eq(other.m_fields, m_fields);
}

// Note: templating these causes many errors
std::vector<DexFieldRef*> get_keys(const FieldSetMap& m) {
  std::vector<DexFieldRef*> m_keys = {};
  for (const auto& f : m) {
    m_keys.emplace_back(f.first);
  }
  return m_keys;
}

std::vector<DexFieldRef*> get_keys(const FieldReadMap& m) {
  std::vector<DexFieldRef*> m_keys = {};
  for (const auto& f : m) {
    m_keys.emplace_back(f.first);
  }
  return m_keys;
}

std::vector<DexMethodRef*> get_keys(const CallMap& m) {
  std::vector<DexMethodRef*> m_keys = {};
  for (const auto& f : m) {
    m_keys.emplace_back(f.first);
  }
  return m_keys;
}

FieldSet path_combine(const FieldSet& main, const FieldSet& other) {
  auto combined_regs = main.regs;
  for (const auto& o_reg : other.regs) {
    auto& reg = combined_regs[o_reg.first];
    for (const auto& o_instr : o_reg.second) {
      reg.insert(o_instr);
    }
  }
  return FieldSet{combined_regs, path_combine(main.set, other.set),
                  path_combine(main.source, other.source)};
}

FieldSet merge(const FieldSet& main, const FieldSet& other) {
  auto source = main.source;
  auto merged_regs = main.regs;
  if (other.regs != main.regs) {
    for (const auto& o_reg : other.regs) {
      auto reg = merged_regs[o_reg.first];
      for (const auto& o_instr : o_reg.second) {
        reg.insert(o_instr);
      }
    }
    if (source == OneReg &&
        (other.source == OneReg || other.source == MultipleReg)) {
      source = MultipleReg;
    }
  } else {
    source = path_combine(main.source, other.source);
  }
  return FieldSet{merged_regs, main.set, source};
}

MethodCall path_combine(const MethodCall& main, const MethodCall& other) {
  auto combined_call_sites = main.call_sites;
  for (std::pair<IRInstruction*, reg_t> o_instr : other.call_sites) {
    combined_call_sites.emplace(o_instr);
  }
  return MethodCall{path_combine(main.call, other.call), combined_call_sites};
}

void FieldWriteRegs::combine_paths(const FieldWriteRegs& other) {
  std::vector<DexFieldRef*> m_keys = get_keys(m_fields);
  for (const auto& field : m_keys) {
    auto field_data = m_fields.find(field);
    auto other_field = other.m_fields.find(field);
    if (other_field == other.m_fields.end()) {
      m_fields[field] = FieldSet{field_data->second.regs, Conditional,
                                 field_data->second.source};
      continue;
    }
    if (other_field->second != field_data->second) {
      m_fields[field] = path_combine(field_data->second, other_field->second);
    }
  }
  for (const auto& other_field : other.m_fields) {
    auto field = m_fields.find(other_field.first);
    if (field == m_fields.end()) {
      m_fields[other_field.first] = (FieldSet){
          other_field.second.regs, Conditional, other_field.second.source};
    }
  }
}

void FieldWriteRegs::merge(const FieldWriteRegs& other) {
  if (other.m_fields.empty()) {
    return;
  }
  std::vector<DexFieldRef*> m_keys = get_keys(m_fields);
  for (const auto& field : m_keys) {
    auto field_data = m_fields.find(field);
    auto other_field = other.m_fields.find(field);
    if (other_field == other.m_fields.end()) {
      continue;
    }
    if (other_field->second != field_data->second) {
      m_fields[field] = ::merge(field_data->second, other_field->second);
    }
  }
  for (const auto& other_field : other.m_fields) {
    if (m_fields.count(other_field.first) == 0) {
      m_fields[other_field.first] = other_field.second;
    }
  }
}

void FieldWriteRegs::add_field(DexFieldRef* field,
                               reg_t reg,
                               IRInstruction* instr) {
  auto seen_field = m_fields.find(field);

  if (seen_field == m_fields.end()) {
    m_fields[field] = FieldSet{{{reg, {instr}}}, AllPaths, OneReg};
  } else {
    auto previous_usage = m_fields[field];
    auto reg_map = previous_usage.regs.find(reg);
    // New register
    if (reg_map == previous_usage.regs.end()) {
      previous_usage.regs.insert({reg, {instr}});
      if (previous_usage.source == OneReg) {
        m_fields[field] = FieldSet{previous_usage.regs, AllPaths, MultipleReg};
      }
    } else {
      reg_map->second.insert(instr);
    }
  }
}

void FieldReads::add_field(DexFieldRef* field) {
  auto exists_field = m_fields.find(field);
  if (exists_field != m_fields.end()) {
    m_fields[field] = AllPaths;
  }
}

// Fields that don't match are inconsistent but ok to have more or less fields
bool FieldReads::consistent_with(const FieldReads& other) {
  for (const auto& read : m_fields) {
    auto o_read = other.m_fields.find(read.first);
    if (o_read == other.m_fields.end()) {
      continue;
    }
    if (o_read->second != read.second) {
      return false;
    }
  }
  return true;
}

void FieldReads::combine_paths(const FieldReads& other) {
  if (other.m_fields.empty()) {
    return;
  }
  auto keys = get_keys(m_fields);
  for (const auto& key : keys) {
    if (other.m_fields.count(key) == 0) {
      m_fields[key] = Conditional;
    }
  }
  for (const auto& o_read : other.m_fields) {
    auto read = m_fields.find(o_read.first);
    if (read == m_fields.end()) {
      m_fields[o_read.first] = Conditional;
    }
  }
}

void FieldReads::merge(const FieldReads& other) {
  if (other.m_fields.empty()) {
    return;
  }
  // Outer path flow holds over inner path flow, so just don't lose any
  for (const auto& o_read : other.m_fields) {
    if (m_fields.count(o_read.first) == 0) {
      m_fields[o_read.first] = o_read.second;
    }
  }
}

bool MethodCalls::consistent_with(const MethodCalls& other) {
  return calls_subset_eq(m_calls, other.m_calls) ||
         calls_subset_eq(other.m_calls, m_calls);
}

void MethodCalls::combine_paths(const MethodCalls& other) {
  if (other.m_calls.empty()) {
    return;
  }
  std::vector<DexMethodRef*> m_keys = get_keys(m_calls);
  for (const auto& call : m_keys) {
    auto call_data = m_calls.find(call);
    auto other_call = other.m_calls.find(call);
    if (other_call == other.m_calls.end()) {
      if (call_data->second.call != Conditional) {
        m_calls[call] = (MethodCall){Conditional, call_data->second.call_sites};
      }
      continue;
    }
    if (other_call->second != call_data->second) {
      m_calls[call] = ::path_combine(call_data->second, other_call->second);
    }
  }
  for (const auto& other_call : other.m_calls) {
    auto call = m_calls.find(other_call.first);
    if (call == m_calls.end()) {
      m_calls[other_call.first] =
          MethodCall{Conditional, other_call.second.call_sites};
    }
  }
}

void MethodCalls::merge(const MethodCalls& other) {
  if (other.m_calls.empty()) {
    return;
  }
  std::vector<DexMethodRef*> m_keys = get_keys(m_calls);
  for (const auto& call : m_keys) {
    auto call_data = m_calls.find(call);
    auto other_call = other.m_calls.find(call);
    if (other_call == other.m_calls.end()) {
      continue;
    }
    if (other_call->second != call_data->second) {
      for (const auto& o_call_site : other_call->second.call_sites) {
        call_data->second.call_sites.insert(o_call_site);
      }
    }
  }
  for (const auto& o_call : other.m_calls) {
    auto m_call = m_calls.find(o_call.first);
    if (m_call == m_calls.end()) {
      m_calls[o_call.first] = o_call.second;
    }
  }
}

void MethodCalls::add_call(DexMethodRef* method,
                           reg_t in_reg,
                           IRInstruction* instr) {
  auto seen_method = m_calls.find(method);
  if (seen_method == m_calls.end()) {
    m_calls[method] = MethodCall{AllPaths, {{instr, in_reg}}};
    return;
  }
  seen_method->second.call_sites.insert({instr, in_reg});
  m_calls[method] = MethodCall{AllPaths, seen_method->second.call_sites};
}

void Escapes::add_array(IRInstruction* instr) {
  via_array_write[instr] = AllPaths;
}

void Escapes::add_return(IRInstruction* instr) {
  via_return = AllPaths;
  return_instrs.insert(instr);
}

void Escapes::add_field_set(DexFieldRef* field,
                            reg_t reg,
                            IRInstruction* instr) {
  auto exists_check = via_field_set.find(field);
  if (exists_check == via_field_set.end()) {
    via_field_set[field] = FieldSet{{{reg, {instr}}}, AllPaths, OneReg};
    return;
  }
  auto e_reg = exists_check->second.regs.find(reg);
  if (e_reg == exists_check->second.regs.end()) {
    exists_check->second.regs.insert({reg, {instr}});
    via_field_set[field] =
        FieldSet{exists_check->second.regs, AllPaths, MultipleReg};
    return;
  }
  e_reg->second.insert(instr);
}

void Escapes::add_dmethod(DexMethodRef* method,
                          reg_t object,
                          IRInstruction* instr) {
  auto exists_check = via_vmethod_call.find(method);
  if (exists_check == via_vmethod_call.end()) {
    via_vmethod_call[method] = MethodCall{AllPaths, {{instr, object}}};
  } else {
    via_vmethod_call[method].call_sites.insert({instr, object});
  }
}

void Escapes::add_smethod(DexMethodRef* method,
                          reg_t object,
                          IRInstruction* instr) {
  auto exists_check = via_smethod_call.find(method);
  if (exists_check == via_smethod_call.end()) {
    via_smethod_call[method] = MethodCall{AllPaths, {{instr, object}}};
  } else {
    via_smethod_call[method].call_sites.insert({instr, object});
  }
}

bool Escapes::consistent_with(const Escapes& other) {
  if (via_return != other.via_return) {
    return false;
  }
  for (const auto& field : via_field_set) {
    auto o_field = other.via_field_set.find(field.first);
    if (o_field == other.via_field_set.end()) {
      continue;
    }
    if (o_field->second != field.second) {
      return false;
    }
  }
  for (const auto& vmethod : via_vmethod_call) {
    auto o_vmethod = other.via_vmethod_call.find(vmethod.first);
    if (o_vmethod == other.via_vmethod_call.end()) {
      continue;
    }
    if (o_vmethod->second != vmethod.second) {
      return false;
    }
  }
  for (const auto& smethod : via_smethod_call) {
    auto o_smethod = other.via_smethod_call.find(smethod.first);
    if (o_smethod == other.via_smethod_call.end()) {
      continue;
    }
    if (o_smethod->second != smethod.second) {
      return false;
    }
  }
  return true;
}

void Escapes::combine_paths(const Escapes& other) {
  if (!return_instrs.empty() || !other.return_instrs.empty()) {
    via_return =
        path_combine(via_return ? via_return.value() : Conditional,
                     other.via_return ? other.via_return.value() : Conditional);
    for (const auto& o_instr : other.return_instrs) {
      return_instrs.insert(o_instr);
    }
  }
  for (auto array : via_array_write) {
    auto other_flow = other.via_array_write.find(array.first);
    if (other_flow == other.via_array_write.end()) {
      array.second = Conditional;
    }
  }
  for (const auto& set : via_field_set) {
    auto o_set = other.via_field_set.find(set.first);
    if (o_set == other.via_field_set.end()) {
      via_field_set[set.first] =
          FieldSet{set.second.regs, Conditional, set.second.source};
      continue;
    }
    if (set.second != o_set->second) {
      via_field_set[set.first] = ::path_combine(set.second, o_set->second);
    }
  }
  for (const auto& o_set : other.via_field_set) {
    auto set = via_field_set.find(o_set.first);
    if (set == via_field_set.end()) {
      via_field_set[o_set.first] =
          FieldSet{o_set.second.regs, Conditional, o_set.second.source};
    }
  }
  for (const auto& call : via_vmethod_call) {
    auto o_call = other.via_vmethod_call.find(call.first);
    if (o_call == other.via_vmethod_call.end()) {
      via_vmethod_call[call.first] =
          MethodCall{Conditional, call.second.call_sites};
      continue;
    }
    if (o_call->second != call.second) {
      via_vmethod_call[call.first] =
          ::path_combine(call.second, o_call->second);
      continue;
    }
  }
  for (const auto& o_call : other.via_vmethod_call) {
    auto call = via_vmethod_call.find(o_call.first);
    if (call == via_vmethod_call.end()) {
      via_vmethod_call[o_call.first] =
          MethodCall{Conditional, o_call.second.call_sites};
    }
  }

  for (const auto& call : via_smethod_call) {
    auto o_call = other.via_smethod_call.find(call.first);
    if (o_call == other.via_smethod_call.end()) {
      via_smethod_call[call.first] =
          MethodCall{Conditional, call.second.call_sites};
      continue;
    }
    if (o_call->second != call.second) {
      via_smethod_call[call.first] =
          ::path_combine(call.second, o_call->second);
      continue;
    }
  }
  for (const auto& o_call : other.via_smethod_call) {
    auto call = via_smethod_call.find(o_call.first);
    if (call == via_smethod_call.end()) {
      via_smethod_call[o_call.first] =
          MethodCall{Conditional, o_call.second.call_sites};
    }
  }
}

void Escapes::merge(const Escapes& other) {
  if (!via_return && other.via_return) {
    via_return = other.via_return;
  }
  for (auto* insn : other.return_instrs) {
    return_instrs.insert(insn);
  }
  for (const auto& i_flow : other.via_array_write) {
    if (via_array_write.count(i_flow.first) == 0) {
      via_array_write[i_flow.first] = i_flow.second;
    }
  }
  for (const auto& o_set : other.via_field_set) {
    auto set = via_field_set.find(o_set.first);
    if (set == via_field_set.end()) {
      via_field_set[o_set.first] = o_set.second;
    }
  }
  for (const auto& o_call : other.via_vmethod_call) {
    auto call = via_vmethod_call.find(o_call.first);
    if (call == via_vmethod_call.end()) {
      via_vmethod_call[o_call.first] = o_call.second;
    }
  }
  for (const auto& o_call : other.via_smethod_call) {
    auto call = via_smethod_call.find(o_call.first);
    if (call == via_smethod_call.end()) {
      via_smethod_call[o_call.first] = o_call.second;
    }
  }
}

TrackedUses::TrackedUses(Tracked kind) : m_tracked_kind(kind) {}
TrackedUses::~TrackedUses() = default;

void TrackedUses::combine_paths(const TrackedUses& other) {
  fields_set.combine_paths(other.fields_set);
  fields_read.combine_paths(other.fields_read);
  method_calls.combine_paths(other.method_calls);
  escapes.combine_paths(other.escapes);
  safe_escapes.combine_paths(other.safe_escapes);
}

std::vector<std::pair<IRInstruction*, reg_t>> Escapes::get_escape_instructions()
    const {
  std::vector<std::pair<IRInstruction*, reg_t>> escapes;
  if (via_return) {
    for (auto i : return_instrs) {
      escapes.emplace_back(std::make_pair(i, i->src(0)));
    }
  }
  for (const auto& f_set : via_field_set) {
    for (const auto& reg_instrs : f_set.second.regs) {
      for (auto i : reg_instrs.second) {
        escapes.emplace_back(std::make_pair(i, reg_instrs.first));
      }
    }
  }

  for (const auto& v_call : via_vmethod_call) {
    for (const auto i_reg : v_call.second.call_sites) {
      escapes.emplace_back(i_reg);
    }
  }
  for (const auto& s_call : via_smethod_call) {
    for (const auto i_reg : s_call.second.call_sites) {
      escapes.emplace_back(i_reg);
    }
  }
  return escapes;
}

void TrackedUses::merge(const TrackedUses& other) {
  fields_set.merge(other.fields_set);
  fields_read.merge(other.fields_read);
  method_calls.merge(other.method_calls);
  escapes.merge(other.escapes);
  safe_escapes.merge(other.safe_escapes);
}

void ObjectUses::combine_paths(const TrackedUses& other) {
  always_assert_log(
      other.m_tracked_kind != Merged,
      "ObjectUses cannot be combined with a MergedUses, check logic at call");
  TrackedUses::combine_paths(other);
  if (reinterpret_cast<const ObjectUses&>(other).created_flow != AllPaths) {
    created_flow = Conditional;
  }
}

void ObjectUses::merge(const TrackedUses& other) {
  always_assert_log(
      other.m_tracked_kind != Merged,
      "ObjectUses cannot be combined with a MergedUses, check logic at call");
  // this uses created_flow supercedes ones from program order later uses
  TrackedUses::merge(other);
}

bool ObjectUses::consistent_with(const TrackedUses& other_tracked) {
  if (other_tracked.m_tracked_kind == Object) {
    auto other = reinterpret_cast<const ObjectUses&>(other_tracked);
    return other.m_ir == m_ir && m_class_used == other.m_class_used;
  } else {
    auto other_merged = reinterpret_cast<const MergedUses&>(other_tracked);
    return other_merged.contains_instr(m_ir) &&
           other_merged.contains_type(m_class_used);
  }
}

MergedUses::MergedUses(const ObjectUses& older, const ObjectUses& newer)
    : TrackedUses(Merged) {
  m_instrs.insert(older.get_po_identity());
  m_instrs.insert(newer.get_po_identity());
  m_classes.insert(older.get_represents_typ());
  m_classes.insert(newer.get_represents_typ());
}

MergedUses::MergedUses(const ObjectUses& other) : TrackedUses(Merged) {
  m_instrs.insert(other.get_po_identity());
  m_classes.insert(other.get_represents_typ());
  m_includes_nullable = true;
}

bool MergedUses::contains_instr(
    const std::shared_ptr<InstructionPOIdentity>& i) const {
  return m_instrs.count(i) == 0;
}

bool MergedUses::contains_type(DexType* c) const {
  return m_classes.count(c) == 0;
}

void MergedUses::combine_paths(const TrackedUses& other) {
  if (other.m_tracked_kind == Object) {
    auto obj_use_other = reinterpret_cast<const ObjectUses&>(other);
    m_instrs.insert(obj_use_other.get_po_identity());
    m_classes.insert(obj_use_other.get_represents_typ());
  } else {
    auto merged_use_other = reinterpret_cast<const MergedUses&>(other);
    m_includes_nullable =
        merged_use_other.m_includes_nullable || m_includes_nullable;
    for (const auto& i : merged_use_other.m_instrs) {
      m_instrs.insert(i);
    }
    for (auto* c : merged_use_other.m_classes) {
      m_classes.insert(c);
    }
  }
  TrackedUses::combine_paths(other);
}

void MergedUses::merge(const TrackedUses& other) {
  if (other.m_tracked_kind == Object) {
    auto obj_use_other = reinterpret_cast<const ObjectUses&>(other);
    m_classes.insert(obj_use_other.get_represents_typ());
    if (!contains_instr(obj_use_other.get_po_identity())) {
      m_instrs.insert(obj_use_other.get_po_identity());
      // A merge between an ObjectUse and a MergedUse from a
      // new instruction always implies different paths were taken to here
      TrackedUses::combine_paths(other);
    } else {
      // We've been here before, but we joined some other instr's path.
      // But we will still merge as though this was still an ObjectUse
      TrackedUses::merge(other);
    }
  } else {
    auto merged_use_other = reinterpret_cast<const MergedUses&>(other);
    for (auto* c : merged_use_other.m_classes) {
      m_classes.insert(c);
    }
    m_includes_nullable =
        merged_use_other.m_includes_nullable || m_includes_nullable;
    std::vector<std::shared_ptr<InstructionPOIdentity>> intersection;
    std::set_intersection(
        m_instrs.begin(), m_instrs.end(), merged_use_other.m_instrs.begin(),
        merged_use_other.m_instrs.end(), std::back_inserter(intersection));

    if (!intersection.empty()) {
      // We have come from some of the same instructions, so merge without paths
      TrackedUses::merge(other);
    } else {
      // We're joining two paths
      TrackedUses::combine_paths(other);
    }
    for (const auto& i : merged_use_other.m_instrs) {
      m_instrs.insert(i);
    }
  }
}

bool MergedUses::consistent_with(const TrackedUses& other_tracked) {
  // May want to subset check the members
  if (other_tracked.m_tracked_kind == Object) {
    auto other = reinterpret_cast<const ObjectUses&>(other_tracked);
    return contains_instr(other.get_po_identity());
  } else {
    auto other_merged = reinterpret_cast<const MergedUses&>(other_tracked);
    std::vector<std::shared_ptr<InstructionPOIdentity>> intersection;
    std::set_intersection(
        m_instrs.begin(), m_instrs.end(), other_merged.m_instrs.begin(),
        other_merged.m_instrs.end(), std::back_inserter(intersection));

    // If our instructions are overlapping, we're consistent
    return !intersection.empty();
  }
}

bool MergedUses::equal(const MergedUses& other) const {
  for (const auto& i : m_instrs) {
    if (!other.contains_instr(i)) {
      return false;
    }
  }
  for (const auto& i : other.m_instrs) {
    if (!contains_instr(i)) {
      return false;
    }
  }
  return true;
}

bool MergedUses::less(const MergedUses& other) const {
  auto m_iterator = m_instrs.begin();
  auto o_iterator = other.m_instrs.begin();
  for (; m_iterator != m_instrs.end(); m_iterator++) {
    for (; o_iterator != other.m_instrs.end(); o_iterator++) {
      if (m_iterator == m_instrs.end()) {
        return false;
      }
      if (*m_iterator < *o_iterator) {
        return true;
      } else if (*m_iterator == *o_iterator) {
        m_iterator++;
        continue;
      }
      return false;
    }
    return false;
  }
  return false;
}

size_t MergedUses::hash() const {
  size_t result;
  for (const auto& i : m_instrs) {
    result = result ^ i->insn->hash();
  }
  return result;
}

std::shared_ptr<TrackedUses> copy_helper(
    const std::shared_ptr<TrackedUses>& orig) {
  if (orig->m_tracked_kind == Object) {
    return std::make_shared<ObjectUses>(static_cast<ObjectUses&>(*orig));
  }
  return std::make_shared<MergedUses>(static_cast<MergedUses&>(*orig));
}

RegisterSet::RegisterSet(RegisterSet const& registers) {
  for (const auto& entry : registers.m_registers) {
    if (entry.second) {
      std::shared_ptr<TrackedUses> uses;
      if (m_all_uses.count(entry.second)) {
        // Due to aliasing amongst registers, the same Use could recur
        uses = *m_all_uses.find(entry.second);
      } else {
        uses = copy_helper(entry.second);
        m_all_uses.insert(uses);
      }
      m_registers[entry.first] = uses;
    }
  }
  for (const auto& entry : registers.m_all_uses) {
    if (m_all_uses.count(entry) == 0) {
      auto uses = copy_helper(entry);
      m_all_uses.insert(uses);
    }
  }
}

bool RegisterSet::consistent_with(const RegisterSet& other) {
  for (const auto& entry : m_registers) {
    auto other_entry = other.get(entry.first);
    if (entry.second && other_entry &&
        entry.second->consistent_with(*other_entry)) {
      continue;
    }
    if (!entry.second && !other_entry) {
      continue;
    }
    return false;
  }
  for (const auto& other_entry : other.m_registers) {
    if (m_registers.count(other_entry.first) == 0 && other_entry.second) {
      return false;
    }
  }
  return true;
}

bool RegisterSet::same_uses(const RegisterSet& other) {
  for (const auto& uses : m_all_uses) {
    auto other_uses = other.m_all_uses.find(uses);
    if (other_uses == other.m_all_uses.end() ||
        !(uses->consistent_with(**other_uses))) {
      return false;
    }
  }
  for (const auto& other_uses : other.m_all_uses) {
    if (m_all_uses.count(other_uses) == 0) {
      return false;
    }
  }
  return true;
}

void RegisterSet::combine_paths(const RegisterSet& other) {
  for (const std::shared_ptr<TrackedUses>& uses : other.m_all_uses) {
    auto local_uses = m_all_uses.find(uses);
    if (local_uses == m_all_uses.end()) {
      if (uses->m_tracked_kind == Object) {
        static_cast<ObjectUses*>(uses.get())->created_flow = Conditional;
      }
      m_all_uses.insert(uses);
      continue;
    }
    // This can't call combine on a Merged and an Object due to set comparison
    (*local_uses)->combine_paths(*uses);
  }
  for (const auto& local_use : m_all_uses) {
    if (other.m_all_uses.count(local_use) == 0) {
      if (local_use->m_tracked_kind == Object) {
        static_cast<ObjectUses*>(local_use.get())->created_flow = Conditional;
      }
    }
  }
}

void RegisterSet::merge_effects(const RegisterSet& other) {
  for (const std::shared_ptr<TrackedUses>& obj_uses : other.m_all_uses) {
    auto local_uses = m_all_uses.find(obj_uses);
    if (local_uses == m_all_uses.end()) {
      m_all_uses.insert(obj_uses);
      continue;
    }
    // This can't call merge on a MergedUses and ObjectUses since
    // TrackedComparer returns false in that circumstance.
    (*local_uses)->merge(*obj_uses);
  }
}

void RegisterSet::merge_registers(const RegisterSet& comes_after,
                                  MergedUsedSet& merge_store) {
  std::unordered_map<reg_t, std::shared_ptr<TrackedUses>> merged_registers;
  for (const auto& before_reg_value : m_registers) {
    auto before_tracked = before_reg_value.second;
    auto is_before_merged =
        before_tracked && before_tracked->m_tracked_kind == Merged;
    auto after_tracked = comes_after.get(before_reg_value.first);
    auto is_after_merged =
        after_tracked && after_tracked->m_tracked_kind == Merged;
    if (!before_tracked && !after_tracked) {
      // Neither RegisterSet has a tracked value, nothing to do
      continue;
    }
    if (!is_before_merged && !is_after_merged) {
      // Both registers point to either ObjectUse, NullableTracked, or nullptr
      if (before_tracked && after_tracked &&
          before_tracked->consistent_with(*after_tracked)) {
        // Both are ObjectUse and consistent
        continue;
      }
      // Value at register could be multiple sorts of tracked value, so merge
      std::shared_ptr<MergedUses> merged;
      if (!after_tracked) {
        // Later register has a nullptr, so lift to NullableTracked and merge
        merged = std::make_shared<MergedUses>(
            static_cast<ObjectUses&>(*before_tracked));
      } else if (!before_tracked) {
        // Previously register contained nullptr, so lift to NullableTracked
        merged = std::make_shared<MergedUses>(
            static_cast<ObjectUses&>(*after_tracked));
      } else {
        // Registers had two Objects from different
        merged = std::make_shared<MergedUses>(
            static_cast<ObjectUses&>(*before_tracked),
            static_cast<ObjectUses&>(*after_tracked));
      }
      merged_registers[before_reg_value.first] = merged;
      merge_store.insert(merged);
      continue;
    }
    if (is_before_merged) {
      // Before_tracked has been merged before
      if (!after_tracked) {
        // Register value will now be tracked and Nullable
        static_cast<MergedUses*>(before_tracked.get())->set_is_nullable();
        continue;
      }
      before_tracked->merge(*after_tracked);
      continue;
    }
    // after_tracked has been merged already
    assert(is_after_merged);
    // First make a copy of the merge to have for this register_set
    auto transferred_use =
        std::make_shared<MergedUses>(static_cast<MergedUses&>(*after_tracked));
    // Then merge into it
    if (before_tracked) {
      transferred_use->merge(*before_tracked);
    } else {
      transferred_use->set_is_nullable();
    }
    merged_registers[before_reg_value.first] = transferred_use;
    m_all_uses.insert(transferred_use);
  }
  // Look for any added register locations in our later register set
  for (const auto& after_reg_value : comes_after.m_registers) {
    if (m_registers.count(after_reg_value.first) == 0) {
      if (!after_reg_value.second) {
        continue;
      }
      std::shared_ptr<MergedUses> transferred_use;
      if (after_reg_value.second->m_tracked_kind == Merged) {
        transferred_use = std::make_shared<MergedUses>(
            static_cast<MergedUses&>(*after_reg_value.second));
        transferred_use->set_is_nullable();
      } else {
        transferred_use = std::make_shared<MergedUses>(
            static_cast<ObjectUses&>(*after_reg_value.second));
        m_all_uses.insert(after_reg_value.second);
      }
      merged_registers[after_reg_value.first] = transferred_use;
      m_all_uses.insert(transferred_use);
      merge_store.insert(transferred_use);
    }
  }
  for (const auto& reg_update : merged_registers) {
    m_registers[reg_update.first] = reg_update.second;
  }
}

std::shared_ptr<ObjectUses> InitLocation::add_init(DexClass* container,
                                                   DexMethod* caller,
                                                   IRInstruction* instr,
                                                   uint32_t block_id,
                                                   uint32_t instruction_count) {
  if (m_inits[container][caller].count(instr) == 0) {
    // We've not seen this instruction initializing our class before
    // So increase count of the number of initializations
    m_count++;
  }
  TRACE(CIC, 8, "Adding init to %s, from instruction %s", SHOW(m_typ),
        SHOW(instr));
  auto usage =
      std::make_shared<ObjectUses>(m_typ, instr, block_id, instruction_count);
  m_inits[container][caller][instr].emplace_back(usage);
  return usage;
}

void InitLocation::update_object(DexClass* container,
                                 DexMethod* caller,
                                 const ObjectUses& obj) {
  m_inits[container][caller][obj.get_instr()] = {
      std::make_shared<ObjectUses>(obj)};
}

void InitLocation::reset_uses_from(DexClass* cls_impl, DexMethod* method) {
  auto class_seen = m_inits.find(cls_impl);
  if (class_seen == m_inits.end()) {
    return;
  }
  auto& method_table = class_seen->second;
  auto method_seen = method_table.find(method);
  if (method_seen == method_table.end()) {
    return;
  }
  method_table.erase(method);
}

void InitLocation::all_uses_from(DexClass* cls_impl,
                                 DexMethod* method,
                                 ObjectUsedSet& set) const {
  const auto methods = m_inits.find(cls_impl);
  if (methods == m_inits.end()) {
    return;
  }
  const auto instructions_uses = methods->second.find(method);
  if (instructions_uses == methods->second.end()) {
    return;
  }
  for (const auto& inst_uses : instructions_uses->second) {
    set.insert(inst_uses.second.begin(), inst_uses.second.end());
  }
}

ClassInitCounter::ClassInitCounter(
    DexType* parent_class,
    const std::unordered_set<DexMethodRef*>& safe_escapes,
    const std::unordered_set<DexClass*>& classes,
    boost::optional<DexString*> optional_method_name)
    : m_optional_method(optional_method_name), m_safe_escapes{safe_escapes} {
  find_children(parent_class, classes);
  TRACE(CIC,
        3,
        "Found %zu children of parent %s",
        m_type_to_inits.size(),
        SHOW(parent_class));
  for (DexClass* current : classes) {
    for (DexMethod* method : current->get_vmethods()) {
      find_uses_within(current, method);
    }
    for (DexMethod* method : current->get_dmethods()) {
      find_uses_within(current, method);
    }
  }
}

void ClassInitCounter::find_children(
    DexType* parent, const std::unordered_set<DexClass*>& classes) {
  for (DexClass* current : classes) {
    if (current->get_super_class() == parent) {
      auto type = current->get_type();
      m_type_to_inits.insert({type, InitLocation(type)});
    }
  }
}

void ClassInitCounter::analyze_block(
    DexClass* container,
    DexMethod* method,
    TypeToInit& type_to_inits,
    const std::unordered_set<IRInstruction*>& tracked_set,
    cfg::Block* prev_block,
    cfg::Block* block) {
  bool first_visit = true;

  if (visited_blocks.count(prev_block) && visited_blocks.count(block)) {
    TRACE(CIC, 8, "Previously seen block %zu", block->id());
    first_visit = false;
    bool same_registers =
        visited_blocks[block]->input_registers.consistent_with(
            visited_blocks[prev_block]->basic_block_registers);
    if (same_registers && visited_blocks[block]->final_result_registers) {
      TRACE(CIC, 8, "Input hasn't changed and there's a result so end");
      return;
    }
    if (same_registers) {
      TRACE(CIC, 8, "Loop detected, providing basic block result as result");
      visited_blocks[block]->final_result_registers =
          visited_blocks[block]->basic_block_registers;
      return;
    }
    TRACE(CIC, 8, "Repeat visit, with inconsistent input, merge registers");
    visited_blocks[block]->input_registers.merge_registers(
        visited_blocks[prev_block]->basic_block_registers,
        m_stored_mergeds[container->get_type()][method]);
  } else if (visited_blocks.count(prev_block)) {
    TRACE(CIC, 8,
          "First visit to %zu, setup visited blocks with input registers",
          block);
    visited_blocks[block] = std::make_shared<RegistersPerBlock>();
    visited_blocks[block]->input_registers =
        visited_blocks[prev_block]->basic_block_registers;
  } else {
    TRACE(CIC, 8, "First visit to first block of method, setup empty register");
    visited_blocks[block] = std::make_shared<RegistersPerBlock>();
  }

  RegisterSet registers = visited_blocks[block]->input_registers;
  uint32_t block_id = block->id();
  uint32_t instruction_count = 0;

  for (const auto& instr : InstructionIterable(block)) {
    auto i = instr.insn;

    auto opcode = i->opcode();
    auto dest = i->has_dest() ? i->dest() : -1;
    // Many instructions are not important to what we track but still require
    // the dest register to be cleared when present. Ones that should keep dest
    // must set this to false.
    bool clear_dest = i->has_dest();
    const auto& srcs = i->srcs();

    if (opcode::is_move_result_any(opcode)) {
      if (!registers.is_empty(RESULT_REGISTER)) {
        registers.insert(dest, registers.get(RESULT_REGISTER));
        registers.clear(RESULT_REGISTER);
        clear_dest = false;
      }
    } else if (opcode::is_a_move(opcode)) {
      if (!registers.is_empty(srcs[0])) {
        registers.insert(dest, registers.get(srcs[0]));
        clear_dest = false;
      }
    } else if (opcode::is_new_instance(opcode)) {
      DexType* typ = i->get_type();
      registers.clear(RESULT_REGISTER);
      if ((tracked_set.empty() || tracked_set.count(i) != 0) &&
          (type_to_inits.count(typ) != 0)) {
        TRACE(CIC, 5, "Adding an init for type %s", SHOW(typ));
        std::shared_ptr<ObjectUses> use = type_to_inits[typ].add_init(
            container, method, i, block_id, instruction_count);
        registers.insert(RESULT_REGISTER, use);
      }
    } else if (opcode::is_an_iput(opcode)) {
      auto field = i->get_field();
      if (!registers.is_empty(srcs[1])) {
        registers.get(srcs[1])->fields_set.add_field(field, srcs[0], i);
      }
      if (!registers.is_empty(srcs[0])) {
        registers.get(srcs[0])->escapes.add_field_set(field, srcs[0], i);
      }
    } else if (opcode::is_an_iget(opcode)) {
      if (!registers.is_empty(srcs[0])) {
        registers.get(srcs[0])->fields_read.add_field(i->get_field());
      }
      registers.clear(RESULT_REGISTER);
    } else if (opcode::is_an_sput(opcode)) {
      auto field = i->get_field();
      if (!registers.is_empty(srcs[0])) {
        registers.get(srcs[0])->escapes.add_field_set(field, srcs[0], i);
      }
    } else if (opcode::is_an_aput(opcode)) {
      if (!registers.is_empty(srcs[0])) {
        registers.get(srcs[0])->escapes.add_array(i);
      }
    } else if (opcode::is_filled_new_array(opcode)) {
      for (const auto src : srcs) {
        if (!registers.is_empty(src)) {
          registers.get(src)->escapes.add_array(i);
        }
      }
    } else if (opcode::is_invoke_static(opcode)) {
      auto curr_method = i->get_method();
      registers.clear(RESULT_REGISTER);
      if (m_optional_method && curr_method->get_name() == m_optional_method) {
        auto ret_typ = curr_method->get_proto()->get_rtype();
        if ((tracked_set.empty() || tracked_set.count(i) != 0) &&
            type_to_inits.count(ret_typ) != 0) {
          std::shared_ptr<ObjectUses> use = type_to_inits[ret_typ].add_init(
              container, method, i, block_id, instruction_count);
          registers.insert(RESULT_REGISTER, use);
        }
      }
      for (const auto src : srcs) {
        if (!registers.is_empty(src)) {
          if (m_safe_escapes.find(curr_method) == m_safe_escapes.end()) {
            registers.get(src)->escapes.add_smethod(curr_method, src, i);
          } else {
            registers.get(src)->safe_escapes.add_smethod(curr_method, src, i);
          }
        }
      }
    } else if (opcode::is_an_invoke(opcode)) {
      auto target_reg = srcs[0];
      auto curr_method = i->get_method();
      if (!registers.is_empty(target_reg)) {
        registers.get(target_reg)
            ->method_calls.add_call(curr_method, target_reg, i);
      }
      for (const auto src : srcs) {
        if (src != target_reg && !registers.is_empty(src)) {
          if (m_safe_escapes.find(curr_method) == m_safe_escapes.end()) {
            registers.get(src)->escapes.add_dmethod(curr_method, src, i);
          } else {
            registers.get(src)->safe_escapes.add_dmethod(curr_method, src, i);
          }
        }
      }
      registers.clear(RESULT_REGISTER);
    } else if (opcode::is_a_return_value(opcode)) {
      if (srcs.size() == 1 && !(registers.is_empty(srcs[0]))) {
        registers.get(srcs[0])->escapes.add_return(i);
      }
    }
    if (clear_dest) {
      registers.clear(dest);
    }
    instruction_count++;
  }

  if (!first_visit) {
    TRACE(CIC, 8, "Not our first visit to %zu, check for different blocks",
          block_id);
    bool same_block =
        visited_blocks[block]->basic_block_registers.consistent_with(registers);
    if (same_block && visited_blocks[block]->final_result_registers) {
      TRACE(CIC, 8, "No change and a final result, go on");
      return;
    } else if (same_block) {
      TRACE(CIC, 8, "No change, no result, move to have a result and end");
      visited_blocks[block]->final_result_registers = std::move(registers);
      return;
    } else {
      TRACE(CIC, 8, "Basic blocks were inconsistent, update registers");
      visited_blocks[block]->basic_block_registers.merge_registers(
          registers, m_stored_mergeds[container->get_type()][method]);
    }
  } else {
    TRACE(CIC, 8, "Our first visit, move in our registers");
    visited_blocks[block]->basic_block_registers = std::move(registers);
  }

  if (block->succs().empty()) {
    TRACE(CIC, 8, "Termination of block %zu", block->id());
    visited_blocks[block]->final_result_registers =
        visited_blocks[block]->basic_block_registers;
    return;
  }

  RegisterSet paths;
  // Book keeping to differentiate first path from an empty registerset
  bool walked_one_path = false;

  for (auto* edge : block->succs()) {
    cfg::Block* next = edge->target();
    TRACE(CIC, 8, "making call from %zu to block %zu", block->id(), next->id());
    analyze_block(container, method, type_to_inits, tracked_set, block, next);
    assert(visited_blocks[next]->final_result_registers);

    TRACE(CIC, 8, "Combining paths after looking at block %zu from %zu",
          block->id(), next->id());
    if (walked_one_path) {
      paths.combine_paths(visited_blocks[next]->final_result_registers.value());
    } else {
      paths = visited_blocks[next]->final_result_registers.value();
      walked_one_path = true;
    }
  }

  TRACE(CIC, 8, "Update effects of walking paths for %zu", block->id());
  visited_blocks[block]->final_result_registers =
      visited_blocks[block]->basic_block_registers;
  visited_blocks[block]->final_result_registers.value().merge_effects(paths);
}

std::pair<ObjectUsedSet, MergedUsedSet> ClassInitCounter::find_uses_of(
    IRInstruction* origin, DexType* typ, DexMethod* method) {
  TypeToInit init_storage;
  init_storage.insert({typ, InitLocation(typ)});
  std::unordered_set<IRInstruction*> tracked{origin};
  DexClass* container = type_class(method->get_class());

  drive_analysis(container, method, "find_uses_of", tracked, init_storage);

  ObjectUsedSet use;
  use.insert(init_storage[typ].get_inits()[container][method][origin][0]);
  return {use, MergedUsedSet()};
}

void ClassInitCounter::drive_analysis(
    DexClass* container,
    DexMethod* method,
    const std::string& analysis,
    const std::unordered_set<IRInstruction*>& tracking,
    TypeToInit& type_to_inits) {
  IRCode* instructions = method->get_code();
  if (instructions == nullptr) {
    return;
  }
  cfg::ScopedCFG graph(instructions);

  cfg::Block* block = graph->entry_block();
  visited_blocks =
      std::unordered_map<cfg::Block*, std::shared_ptr<RegistersPerBlock>>();

  TRACE(CIC, 5, "starting %s analysis for method %s.%s with %zu blocks\n",
        analysis.c_str(), SHOW(container), SHOW(method), graph->num_blocks());

  analyze_block(container, method, type_to_inits, tracking, nullptr, block);

  auto& merged_set = m_stored_mergeds[container->get_type()][method];
  // This loop collects the results of all ObjectUses and MergedUses encountered
  // in the forwards analysis, which has been merged bottom up to coalesce the
  // final full possible results from this method across all encountered tracked
  // objects.
  // In the future, this may not be necessary with both controlled descent
  // through non-back edges first in the traversal combined with switching to a
  // loop implementation rather than a recursive one.
  for (const auto& use :
       visited_blocks[block]->final_result_registers.value().m_all_uses) {
    if (use->m_tracked_kind == Object) {
      type_to_inits[static_cast<ObjectUses&>(*use).get_represents_typ()]
          .update_object(container, method, static_cast<ObjectUses&>(*use));
    } else {
      merged_set.insert(
          std::make_shared<MergedUses>(static_cast<MergedUses&>(*use)));
    }
  }
}

void ClassInitCounter::find_uses_within(DexClass* container,
                                        DexMethod* method) {
  DexType* container_type = container->get_type();
  for (auto& t_init : m_type_to_inits) {
    t_init.second.reset_uses_from(container, method);
  }
  if (m_stored_mergeds.count(container_type) != 0) {
    m_stored_mergeds[container_type].erase(method);
  }
  std::unordered_set<IRInstruction*> empty;
  drive_analysis(container, method, "find_uses_within", empty, m_type_to_inits);
}

std::pair<ObjectUsedSet, MergedUsedSet> ClassInitCounter::all_uses_from(
    DexType* container, DexMethod* method) {
  MergedUsedSet merged_set;
  const auto container_methods = m_stored_mergeds.find(container);
  if (container_methods != m_stored_mergeds.end()) {
    const auto methods_uses = container_methods->second.find(method);
    if (methods_uses != container_methods->second.end()) {
      merged_set.insert(methods_uses->second.begin(),
                        methods_uses->second.end());
    }
  }

  ObjectUsedSet object_set;
  for (const auto& typ_init : m_type_to_inits) {
    typ_init.second.all_uses_from(type_class(container), method, object_set);
  }

  return std::make_pair(object_set, merged_set);
}

// This is generating an almost json representation, but may have extra ,s
std::string ClassInitCounter::debug_show_table() {
  std::stringstream result;
  result << "[";
  for (const auto& type_entry : m_type_to_inits) {
    result << "{\"type\" : \"" << type_entry.first->str() << "\", "
           << "\"init\" : " << show(type_entry.second) << "}";
  }
  result << "]";
  return result.str();
}
