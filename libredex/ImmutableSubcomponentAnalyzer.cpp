/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ImmutableSubcomponentAnalyzer.h"

#include <sstream>
#include <unordered_map>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>

#include "AbstractDomain.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "FixpointIterators.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

std::string AccessPath::to_string() const {
  std::ostringstream out;
  out << *this;
  return out.str();
}

size_t hash_value(const AccessPath& path) {
  size_t seed = boost::hash_range(path.getters().begin(), path.getters().end());
  boost::hash_combine(seed, path.parameter());
  return seed;
}

bool operator==(const AccessPath& x, const AccessPath& y) {
  return x.parameter() == y.parameter() && x.getters() == y.getters();
}

std::ostream& operator<<(std::ostream& o, const AccessPath& path) {
  o << "p" << path.parameter();
  for (DexMethodRef* method : path.getters()) {
    o << "." << method->get_name()->str() << "()";
  }
  return o;
}

namespace isa_impl {

// The base abstract domain is the flat lattice (aka the lattice of constants)
// over access paths.
class AbstractAccessPath final : public AbstractValue<AbstractAccessPath> {
 public:
  AbstractAccessPath() = default;

  explicit AbstractAccessPath(const AccessPath& path) : m_path(path) {}

  void clear() override { m_path.m_getters.clear(); }

  AbstractValueKind kind() const override { return AbstractValueKind::Value; }

  bool leq(const AbstractAccessPath& other) const override {
    return equals(other);
  }

  bool equals(const AbstractAccessPath& other) const override {
    return m_path == other.m_path;
  }

  AbstractValueKind join_with(const AbstractAccessPath& other) override {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    clear();
    return AbstractValueKind::Top;
  }

  AbstractValueKind widen_with(const AbstractAccessPath& other) override {
    return join_with(other);
  }

  AbstractValueKind meet_with(const AbstractAccessPath& other) override {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    clear();
    return AbstractValueKind::Bottom;
  }

  AbstractValueKind narrow_with(const AbstractAccessPath& other) override {
    return meet_with(other);
  }

  AccessPath access_path() const { return m_path; }

  void append(DexMethodRef* getter) { m_path.m_getters.push_back(getter); }

 private:
  AccessPath m_path;
};

class AbstractAccessPathDomain final
    : public AbstractDomainScaffolding<AbstractAccessPath,
                                       AbstractAccessPathDomain> {
 public:
  // Inherit constructors from AbstractDomainScaffolding.
  using AbstractDomainScaffolding::AbstractDomainScaffolding;

  // Constructor inheritance is buggy in some versions of gcc, hence the
  // redefinition of the default constructor.
  AbstractAccessPathDomain() { set_to_top(); }

  explicit AbstractAccessPathDomain(const AccessPath& path) {
    set_to_value(AbstractAccessPath(path));
  }

  void append(DexMethodRef* getter) {
    if (kind() == AbstractValueKind::Value) {
      get_value()->append(getter);
    }
  }

  boost::optional<AccessPath> access_path() const {
    return (kind() == AbstractValueKind::Value)
               ? boost::optional<AccessPath>(get_value()->access_path())
               : boost::none;
  }

  static AbstractAccessPathDomain bottom() {
    return AbstractAccessPathDomain(AbstractValueKind::Bottom);
  }

  static AbstractAccessPathDomain top() {
    return AbstractAccessPathDomain(AbstractValueKind::Top);
  }
};

using namespace std::placeholders;

using register_t = uint32_t;

// We use this special register to denote the result of a method invocation.
// Operations that may throw an exception also store their result in that
// special register, but they are transparent for this particular analysis.
register_t RESULT_REGISTER = std::numeric_limits<register_t>::max();

using AbstractAccessPathEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, AbstractAccessPathDomain>;

class Analyzer final
    : public MonotonicFixpointIterator<cfg::GraphInterface,
                                       AbstractAccessPathEnvironment> {
 public:
  using NodeId = cfg::Block*;

  Analyzer(const cfg::ControlFlowGraph& cfg,
           std::function<bool(DexMethodRef*)> is_immutable_getter)
      : MonotonicFixpointIterator(cfg, cfg.blocks().size()),
        m_cfg(cfg),
        m_is_immutable_getter(is_immutable_getter) {}

  void analyze_node(
      const NodeId& node,
      AbstractAccessPathEnvironment* current_state) const override {
    for (const MethodItemEntry& mie : *node) {
      if (mie.type == MFLOW_OPCODE) {
        analyze_instruction(mie.insn, current_state);
      }
    }
  }

  AbstractAccessPathEnvironment analyze_edge(
      const EdgeId&, const AbstractAccessPathEnvironment& exit_state_at_source)
      const override {
    return exit_state_at_source;
  }

  void analyze_instruction(IRInstruction* insn,
                           AbstractAccessPathEnvironment* current_state) const {
    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_WIDE: {
      // These pseudo-operations have already been analyzed during the
      // initialization of the fixpoint iteration. There's nothing more to do.
      break;
    }
    case OPCODE_MOVE_OBJECT: {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
      break;
    }
    case OPCODE_MOVE_RESULT_OBJECT: {
      current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
      break;
    }
    case OPCODE_INVOKE_VIRTUAL: {
      // This analysis is only concerned with virtual calls to getter methods.
      DexMethodRef* dex_method = insn->get_method();
      auto proto = dex_method->get_proto();
      if (is_object(proto->get_rtype()) && proto->get_args()->size() == 0 &&
          m_is_immutable_getter(dex_method)) {
        // Note that a getter takes no arguments and returns an object.
        AbstractAccessPathDomain abs_path = current_state->get(insn->src(0));
        abs_path.append(dex_method);
        current_state->set(RESULT_REGISTER, abs_path);
      } else {
        current_state->set(RESULT_REGISTER, AbstractAccessPathDomain::top());
      }
      break;
    }
    default: {
      // All other instructions are transparent for this analysis. We just need
      // to clobber the destination registers in the abstract environment.
      if (insn->dests_size() > 0) {
        current_state->set(insn->dest(), AbstractAccessPathDomain::top());
        if (insn->dest_is_wide()) {
          current_state->set(insn->dest() + 1, AbstractAccessPathDomain::top());
        }
      }
      // We need to invalidate RESULT_REGISTER if the instruction writes into
      // this register.
      if (insn->has_move_result()) {
        current_state->set(RESULT_REGISTER, AbstractAccessPathDomain::top());
      }
    }
    }
  }

  boost::optional<AccessPath> get_access_path(size_t reg,
                                              IRInstruction* insn) const {
    auto it = m_environments.find(insn);
    if (it == m_environments.end()) {
      return boost::none;
    }
    AbstractAccessPathDomain abs_path = it->second.get(reg);
    return abs_path.access_path();
  }

  void populate_environments() {
    // We reserve enough space for the map in order to avoid repeated rehashing
    // during the computation.
    m_environments.reserve(m_cfg.blocks().size() * 16);
    for (cfg::Block* block : m_cfg.blocks()) {
      AbstractAccessPathEnvironment current_state = get_entry_state_at(block);
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        m_environments.emplace(insn, current_state);
        analyze_instruction(insn, &current_state);
      }
    }
  }

 private:
  const cfg::ControlFlowGraph& m_cfg;
  std::function<bool(DexMethodRef*)> m_is_immutable_getter;
  std::unordered_map<IRInstruction*, AbstractAccessPathEnvironment>
      m_environments;
};

} // namespace isa_impl

ImmutableSubcomponentAnalyzer::~ImmutableSubcomponentAnalyzer() {}

ImmutableSubcomponentAnalyzer::ImmutableSubcomponentAnalyzer(
    DexMethod* dex_method,
    std::function<bool(DexMethodRef*)> is_immutable_getter) {
  IRCode* code = dex_method->get_code();
  if (code == nullptr) {
    return;
  }
  code->build_cfg();
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();
  m_analyzer = std::make_unique<isa_impl::Analyzer>(cfg, is_immutable_getter);

  // We set up the initial environment by going over the LOAD_PARAM_*
  // pseudo-instructions.
  auto init = isa_impl::AbstractAccessPathEnvironment::top();
  size_t parameter = 0;
  for (const auto& mie :
       InstructionIterable(code->get_param_instructions())) {
    switch (mie.insn->opcode()) {
    case IOPCODE_LOAD_PARAM_OBJECT: {
      init.set(mie.insn->dest(),
               isa_impl::AbstractAccessPathDomain(AccessPath(parameter)));
      break;
    }
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_WIDE: {
      // We skip parameters that are not references.
      break;
    }
    default: {
      always_assert_log(false, "Unexpected instruction '%s'", SHOW(mie.insn));
    }
    }
    ++parameter;
  }

  m_analyzer->run(init);
  m_analyzer->populate_environments();
}

boost::optional<AccessPath> ImmutableSubcomponentAnalyzer::get_access_path(
    size_t reg, IRInstruction* insn) const {
  if (m_analyzer == nullptr) {
    return boost::none;
  }
  return m_analyzer->get_access_path(reg, insn);
}
