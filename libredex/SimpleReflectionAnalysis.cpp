/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SimpleReflectionAnalysis.h"

#include <iomanip>
#include <unordered_map>

#include <boost/optional.hpp>

#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRInstructionAnalyzer.h"
#include "IROpcode.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "Show.h"

using namespace sparta;

namespace sra {

bool operator==(const AbstractObject& x, const AbstractObject& y) {
  if (x.kind != y.kind) {
    return false;
  }
  switch (x.kind) {
  case OBJECT:
  case CLASS: {
    return x.dex_type == y.dex_type;
  }
  case STRING: {
    return x.dex_string == y.dex_string;
  }
  case FIELD:
  case METHOD: {
    return x.dex_type == y.dex_type && x.dex_string == y.dex_string;
  }
  }
}

bool operator!=(const AbstractObject& x, const AbstractObject& y) {
  return !(x == y);
}

std::ostream& operator<<(std::ostream& out, const AbstractObject& x) {
  switch (x.kind) {
  case OBJECT: {
    out << "OBJECT{" << SHOW(x.dex_type) << "}";
    break;
  }
  case STRING: {
    const std::string& str = x.dex_string->str();
    if (str.empty()) {
      out << "\"\"";
    } else {
      out << std::quoted(str);
    }
    break;
  }
  case CLASS: {
    out << "CLASS{" << SHOW(x.dex_type) << "}";
    break;
  }
  case FIELD: {
    out << "FIELD{" << SHOW(x.dex_type) << ":" << SHOW(x.dex_string) << "}";
    break;
  }
  case METHOD: {
    out << "METHOD{" << SHOW(x.dex_type) << ":" << SHOW(x.dex_string) << "}";
    break;
  }
  }
  return out;
}

namespace impl {

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

using AbstractObjectDomain = ConstantAbstractDomain<AbstractObject>;

using AbstractObjectEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, AbstractObjectDomain>;

class Analyzer final : public IRInstructionAnalyzer<AbstractObjectEnvironment> {
 public:
  explicit Analyzer(const cfg::ControlFlowGraph& cfg)
      : IRInstructionAnalyzer(cfg) {
    MonotonicFixpointIterator::run(AbstractObjectEnvironment::top());
    populate_environments(cfg);
  }

  void analyze_instruction(
      IRInstruction* insn,
      AbstractObjectEnvironment* current_state) const override {
    switch (insn->opcode()) {
    case OPCODE_MOVE_OBJECT: {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT: {
      current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
      break;
    }
    case OPCODE_CONST_STRING: {
      current_state->set(
          RESULT_REGISTER,
          AbstractObjectDomain(AbstractObject(insn->get_string())));
      break;
    }
    case OPCODE_CONST_CLASS: {
      current_state->set(
          RESULT_REGISTER,
          AbstractObjectDomain(AbstractObject(CLASS, insn->get_type())));
      break;
    }
    case OPCODE_CHECK_CAST: {
      current_state->set(RESULT_REGISTER, current_state->get(insn->src(0)));
      // Note that this is sound. In a concrete execution, if the check-cast
      // operation fails, an exception is thrown and the control point following
      // the check-cast becomes unreachable, which corresponds to _|_ in the
      // abstract domain. Any abstract state is a sound approximation of _|_.
      break;
    }
    case OPCODE_NEW_INSTANCE:
    case OPCODE_NEW_ARRAY:
    case OPCODE_FILLED_NEW_ARRAY: {
      current_state->set(
          RESULT_REGISTER,
          AbstractObjectDomain(AbstractObject(OBJECT, insn->get_type())));
      break;
    }
    case OPCODE_INVOKE_VIRTUAL: {
      auto receiver = current_state->get(insn->src(0)).get_constant();
      if (!receiver) {
        default_semantics(insn, current_state);
        break;
      }
      process_virtual_call(insn, *receiver, current_state);
      break;
    }
    case OPCODE_INVOKE_STATIC: {
      if (insn->get_method() == m_for_name) {
        auto class_name = current_state->get(insn->src(0)).get_constant();
        if (class_name && class_name->kind == STRING) {
          auto internal_name = DexString::make_string(
            JavaNameUtil::external_to_internal(class_name->dex_string->str()));
          current_state->set(
              RESULT_REGISTER,
              AbstractObjectDomain(AbstractObject(
                  CLASS, DexType::make_type(internal_name))));
          break;
        }
      }
      default_semantics(insn, current_state);
      break;
    }
    default: { default_semantics(insn, current_state); }
    }
  }

  boost::optional<AbstractObject> get_abstract_object(
      size_t reg, IRInstruction* insn) const {
    auto it = m_environments.find(insn);
    if (it == m_environments.end()) {
      return boost::none;
    }
    return it->second.get(reg).get_constant();
  }

 private:
  void default_semantics(IRInstruction* insn,
                         AbstractObjectEnvironment* current_state) const {
    // For instructions that are transparent for this analysis, we just need
    // to clobber the destination registers in the abstract environment. Note
    // that this also covers the MOVE_RESULT_* and MOVE_RESULT_PSEUDO_*
    // instructions following operations that are not considered by this
    // analysis. Hence, the effect of those operations is correctly abstracted
    // away regardless of the size of the destination register.
    if (insn->dests_size() > 0) {
      current_state->set(insn->dest(), AbstractObjectDomain::top());
      if (insn->dest_is_wide()) {
        current_state->set(insn->dest() + 1, AbstractObjectDomain::top());
      }
    }
    // We need to invalidate RESULT_REGISTER if the instruction writes into
    // this register.
    if (insn->has_move_result()) {
      current_state->set(RESULT_REGISTER, AbstractObjectDomain::top());
    }
  }

  DexString* get_dex_string_from_insn(AbstractObjectEnvironment* current_state,
                                      IRInstruction* insn,
                                      register_t reg) const {
    auto element_name = current_state->get(insn->src(reg)).get_constant();
    if (element_name && element_name->kind == STRING) {
      return element_name->dex_string;
    } else {
      return nullptr;
    }
  }

  void process_virtual_call(IRInstruction* insn,
                            const AbstractObject& receiver,
                            AbstractObjectEnvironment* current_state) const {
    DexMethodRef* callee = insn->get_method();
    switch (receiver.kind) {
    case OBJECT: {
      if (callee == m_get_class) {
        current_state->set(
            RESULT_REGISTER,
            AbstractObjectDomain(AbstractObject(CLASS, receiver.dex_type)));
        return;
      }
      break;
    }
    case STRING: {
      if (callee == m_get_class) {
        current_state->set(
            RESULT_REGISTER,
            AbstractObjectDomain(AbstractObject(CLASS, get_string_type())));
        return;
      }
      break;
    }
    case CLASS: {
      AbstractObjectKind element_kind;
      DexString* element_name = nullptr;
      if (callee == m_get_method || callee == m_get_declared_method) {
        element_kind = METHOD;
        element_name = get_dex_string_from_insn(current_state, insn, 1);
      } else if (m_ctor_lookup_vmethods.count(callee) > 0) {
        element_kind = METHOD;
        // Hard code the <init> method name, to continue on treating this as no
        // different than a method.
        element_name = DexString::get_string("<init>");
      } else if (callee == m_get_field || callee == m_get_declared_field) {
        element_kind = FIELD;
        element_name = get_dex_string_from_insn(current_state, insn, 1);
      }
      if (element_name == nullptr) {
        break;
      }
      current_state->set(RESULT_REGISTER,
                         AbstractObjectDomain(AbstractObject(
                             element_kind, callee->get_class(), element_name)));
      return;
    }
    case FIELD:
    case METHOD: {
      if ((receiver.kind == FIELD && callee == m_get_field_name) ||
          (receiver.kind == METHOD && callee == m_get_method_name)) {
        current_state->set(
            RESULT_REGISTER,
            AbstractObjectDomain(AbstractObject(receiver.dex_string)));
        return;
      }
      break;
    }
    }
    default_semantics(insn, current_state);
  }

  // After the fixpoint iteration completes, we replay the analysis on all
  // blocks and we cache the abstract state at each instruction. This cache is
  // used by get_abstract_object() to query the state of a register at a given
  // instruction. Since we use an abstract domain based on Patricia trees, the
  // memory footprint of storing the abstract state at each program point is
  // small.
  void populate_environments(const cfg::ControlFlowGraph& cfg) {
    // We reserve enough space for the map in order to avoid repeated rehashing
    // during the computation.
    m_environments.reserve(cfg.blocks().size() * 16);
    for (cfg::Block* block : cfg.blocks()) {
      AbstractObjectEnvironment current_state = get_entry_state_at(block);
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        m_environments.emplace(insn, current_state);
        analyze_instruction(insn, &current_state);
      }
    }
  }

  std::unordered_map<IRInstruction*, AbstractObjectEnvironment> m_environments;
  DexMethodRef* m_get_class{DexMethod::make_method(
      "Ljava/lang/Object;", "getClass", {}, "Ljava/lang/Class;")};
  DexMethodRef* m_get_method{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getMethod",
                             {"Ljava/lang/String;", "[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Method;")};
  DexMethodRef* m_get_declared_method{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredMethod",
                             {"Ljava/lang/String;", "[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Method;")};
  DexMethodRef* m_get_constructor{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getConstructor",
                             {"[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Constructor;")};
  DexMethodRef* m_get_declared_constructor{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredConstructor",
                             {"[Ljava/lang/Class;"},
                             "Ljava/lang/reflect/Constructor;")};
  DexMethodRef* m_get_constructors{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getConstructors",
                             {},
                             "[Ljava/lang/reflect/Constructor;")};
  DexMethodRef* m_get_declared_constructors{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredConstructors",
                             {},
                             "[Ljava/lang/reflect/Constructor;")};
  // Set of vmethods on java.lang.Class that can find constructors.
  std::unordered_set<DexMethodRef*> m_ctor_lookup_vmethods{{
      m_get_constructor,
      m_get_declared_constructor,
      m_get_constructors,
      m_get_declared_constructors,
  }};
  DexMethodRef* m_get_field{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getField",
                             {"Ljava/lang/String;"},
                             "Ljava/lang/reflect/Field;")};
  DexMethodRef* m_get_declared_field{
      DexMethod::make_method("Ljava/lang/Class;",
                             "getDeclaredField",
                             {"Ljava/lang/String;"},
                             "Ljava/lang/reflect/Field;")};
  DexMethodRef* m_get_method_name{DexMethod::make_method(
      "Ljava/lang/reflect/Method;", "getName", {}, "Ljava/lang/String;")};
  DexMethodRef* m_get_field_name{DexMethod::make_method(
      "Ljava/lang/reflect/Field;", "getName", {}, "Ljava/lang/String;")};
  DexMethodRef* m_for_name{DexMethod::make_method("Ljava/lang/Class;",
                                                  "forName",
                                                  {"Ljava/lang/String;"},
                                                  "Ljava/lang/Class;")};
};

} // namespace impl

SimpleReflectionAnalysis::~SimpleReflectionAnalysis() {}

SimpleReflectionAnalysis::SimpleReflectionAnalysis(DexMethod* dex_method) {
  IRCode* code = dex_method->get_code();
  if (code == nullptr) {
    return;
  }
  code->build_cfg(/* editable */ false);
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();
  m_analyzer = std::make_unique<impl::Analyzer>(cfg);
}

boost::optional<AbstractObject> SimpleReflectionAnalysis::get_abstract_object(
    size_t reg, IRInstruction* insn) const {
  if (m_analyzer == nullptr) {
    return boost::none;
  }
  return m_analyzer->get_abstract_object(reg, insn);
}

} // namespace sra
