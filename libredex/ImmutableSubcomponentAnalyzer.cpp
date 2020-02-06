/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ImmutableSubcomponentAnalyzer.h"

#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/optional.hpp>

#include "AbstractDomain.h"
#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "Resolver.h"

using namespace sparta;

std::string AccessPath::to_string() const {
  std::ostringstream out;
  out << *this;
  return out.str();
}

size_t hash_value(const AccessPath& path) {
  size_t seed = boost::hash_range(path.getters().begin(), path.getters().end());
  boost::hash_combine(seed, path.parameter());
  boost::hash_combine(seed, path.kind());
  boost::hash_combine(seed, path.field());
  return seed;
}

bool operator==(const AccessPath& x, const AccessPath& y) {
  return x.parameter() == y.parameter() && x.kind() == y.kind() &&
         x.field() == y.field() && x.getters() == y.getters();
}

bool operator!=(const AccessPath& x, const AccessPath& y) { return !(x == y); }

std::ostream& operator<<(std::ostream& o, const AccessPath& path) {
  auto kind = path.kind();
  o << (kind == AccessPathKind::Parameter ? "p" : "v");
  o << path.parameter();
  if (kind == AccessPathKind::FinalField) {
    o << "." << show(path.field());
  }
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
};

using namespace std::placeholders;

using namespace ir_analyzer;

// Used for new-instance handling. Shouldn't collide with anything.
reg_t UNKNOWN_REGISTER = RESULT_REGISTER - 1;

using AbstractAccessPathEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, AbstractAccessPathDomain>;

class Analyzer final : public BaseIRAnalyzer<AbstractAccessPathEnvironment> {
 public:
  Analyzer(const cfg::ControlFlowGraph& cfg,
           std::function<bool(DexMethodRef*)> is_immutable_getter,
           const std::unordered_set<reg_t>& allowed_locals)
      : BaseIRAnalyzer<AbstractAccessPathEnvironment>(cfg),
        m_cfg(cfg),
        m_is_immutable_getter(std::move(is_immutable_getter)),
        m_allowed_locals(allowed_locals) {}

  bool is_local_analyzable(reg_t reg) const {
    return m_allowed_locals.count(reg) > 0;
  }

  void analyze_instruction(
      const IRInstruction* insn,
      AbstractAccessPathEnvironment* current_state) const override {
    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_WIDE: {
      // These pseudo-operations have already been analyzed during the
      // initialization of the fixpoint iteration. There's nothing more to do.
      break;
    }
    case OPCODE_CHECK_CAST: {
      // Slightly different in IR land than dex bytecode. Treat this as a move,
      // which will be followed up by a IOPCODE_MOVE_RESULT_PSEUDO_OBJECT (also
      // a move).
      current_state->set(RESULT_REGISTER, current_state->get(insn->src(0)));
      break;
    }
    case OPCODE_IGET_OBJECT: {
      auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
      auto source = insn->src(0);
      if (field == nullptr || !is_local_analyzable(source) ||
          !is_final(field)) {
        current_state->set(RESULT_REGISTER, AbstractAccessPathDomain::top());
      } else {
        AccessPath p{AccessPathKind::FinalField, source, field, {}};
        current_state->set(RESULT_REGISTER, AbstractAccessPathDomain(p));
      }
      break;
    }
    case OPCODE_NEW_INSTANCE: {
      // Fill in the state in two steps, completed with the next IR instruction.
      AccessPath p{AccessPathKind::Local, UNKNOWN_REGISTER};
      current_state->set(RESULT_REGISTER, AbstractAccessPathDomain(p));
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
      auto dest = insn->dest();
      auto result_domain = current_state->get(RESULT_REGISTER);
      current_state->set(dest, result_domain);
      // Special handling for when this follows new-instance.
      if (result_domain.is_value() && result_domain.access_path()) {
        auto path = *result_domain.access_path();
        if (path.parameter() == UNKNOWN_REGISTER) {
          // Fill in the actual local var that was unknown during new-instance.
          if (is_local_analyzable(dest)) {
            AccessPath p{AccessPathKind::Local, dest};
            current_state->set(dest, AbstractAccessPathDomain(p));
          } else {
            current_state->set(dest, AbstractAccessPathDomain::top());
          }
        }
      }
      break;
    }
    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT: {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
      break;
    }
    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT: {
      auto dest = insn->dest();
      auto result_domain = current_state->get(RESULT_REGISTER);
      if (!result_domain.is_value() && is_local_analyzable(dest)) {
        // Allow this register to be the starting point of further analysis.
        auto domain =
            AbstractAccessPathDomain(AccessPath(AccessPathKind::Local, dest));
        current_state->set(dest, domain);
      } else {
        current_state->set(dest, result_domain);
      }
      break;
    }
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_VIRTUAL: {
      // This analysis is only concerned with instance methods (i.e. not static)
      DexMethodRef* dex_method = insn->get_method();
      auto proto = dex_method->get_proto();
      auto supported_return_type = type::is_object(proto->get_rtype()) ||
                                   type::is_primitive(proto->get_rtype());
      if (supported_return_type && proto->get_args()->size() == 0 &&
          m_is_immutable_getter(dex_method)) {
        // Note that a getter takes no arguments.
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
      if (insn->has_dest()) {
        current_state->set(insn->dest(), AbstractAccessPathDomain::top());
        if (insn->dest_is_wide()) {
          current_state->set(insn->dest() + 1, AbstractAccessPathDomain::top());
        }
      }
      // We need to invalidate RESULT_REGISTER if the instruction writes into
      // this register.
      if (insn->has_move_result_any()) {
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

  std::set<size_t> find_access_path_registers(
      const AbstractAccessPathEnvironment& env,
      const AccessPath& path_to_find) const {
    if (!env.is_value()) {
      return {};
    }
    std::set<size_t> res;
    auto& bindings = env.bindings();
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
      auto domain = it->second;
      if (domain.access_path()) {
        auto path = *domain.access_path();
        if (path_to_find == path && it->first != RESULT_REGISTER) {
          res.emplace(it->first);
        }
      }
    }
    return res;
  }

  std::set<size_t> find_access_path_registers(IRInstruction* insn,
                                              const AccessPath& path) const {
    auto it = m_environments.find(insn);
    if (it == m_environments.end()) {
      return {};
    }
    auto env = it->second;
    return find_access_path_registers(env, path);
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

  BindingSnapshot get_known_access_path_bindings(
      const AbstractAccessPathEnvironment& env) {
    BindingSnapshot ret;
    if (env.kind() == AbstractValueKind::Value) {
      const auto& bindings = env.bindings();
      for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        auto domain = it->second;
        if (domain.access_path()) {
          auto path = *domain.access_path();
          if (it->first != RESULT_REGISTER) {
            ret.emplace(it->first, path);
          }
        }
      }
    }
    return ret;
  }

  std::unordered_map<cfg::BlockId, BlockStateSnapshot>
  get_block_state_snapshot() {
    std::unordered_map<cfg::BlockId, BlockStateSnapshot> ret;
    for (NodeId block : m_cfg.blocks()) {
      auto entry_state = get_entry_state_at(block);
      auto exit_state = get_exit_state_at(block);
      BlockStateSnapshot snapshot = {
          get_known_access_path_bindings(entry_state),
          get_known_access_path_bindings(exit_state)};
      ret.emplace(block->id(), snapshot);
    }
    return ret;
  }

 private:
  const cfg::ControlFlowGraph& m_cfg;
  std::function<bool(DexMethodRef*)> m_is_immutable_getter;
  std::unordered_map<IRInstruction*, AbstractAccessPathEnvironment>
      m_environments;
  const std::unordered_set<reg_t> m_allowed_locals;
};

} // namespace isa_impl

ImmutableSubcomponentAnalyzer::~ImmutableSubcomponentAnalyzer() {}

// Determine any unambigious registers that can be the starting point for
// analysis. For example, a register that is a dest exactly once can be
// considered an AccessPath, much like param registers (this should not break
// existing AccessPath comparison/equality checks).
std::unordered_set<reg_t> compute_unambiguous_registers(IRCode* code) {
  std::unordered_map<reg_t, size_t> dest_freq;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      auto dest = insn->dest();
      dest_freq[dest] = dest_freq[dest] + 1;
    }
  }
  std::unordered_set<reg_t> unambiguous;
  for (const auto& pair : dest_freq) {
    if (pair.second == 1) {
      unambiguous.emplace(pair.first);
    }
  }
  return unambiguous;
}

ImmutableSubcomponentAnalyzer::ImmutableSubcomponentAnalyzer(
    DexMethod* dex_method,
    const std::function<bool(DexMethodRef*)>& is_immutable_getter) {
  IRCode* code = dex_method->get_code();
  if (code == nullptr) {
    return;
  }
  code->build_cfg(/* editable */ false);
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();
  std::unordered_set<reg_t> unambiguous = compute_unambiguous_registers(code);
  m_analyzer = std::make_unique<isa_impl::Analyzer>(
      cfg, is_immutable_getter, unambiguous);

  // We set up the initial environment by going over the LOAD_PARAM_*
  // pseudo-instructions.
  auto init = isa_impl::AbstractAccessPathEnvironment::top();
  size_t parameter = 0;
  for (const auto& mie : InstructionIterable(code->get_param_instructions())) {
    switch (mie.insn->opcode()) {
    case IOPCODE_LOAD_PARAM_OBJECT: {
      init.set(mie.insn->dest(),
               isa_impl::AbstractAccessPathDomain(
                   AccessPath(AccessPathKind::Parameter, parameter)));
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

std::set<size_t> ImmutableSubcomponentAnalyzer::find_access_path_registers(
    IRInstruction* insn, const AccessPath& path) const {
  if (m_analyzer == nullptr) {
    return {};
  }
  return m_analyzer->find_access_path_registers(insn, path);
}

std::unordered_map<cfg::BlockId, BlockStateSnapshot>
ImmutableSubcomponentAnalyzer::get_block_state_snapshot() const {
  if (m_analyzer == nullptr) {
    return {{}};
  }
  return m_analyzer->get_block_state_snapshot();
}
