/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRTypeChecker.h"

#include "DexUtil.h"
#include "Match.h"
#include "Show.h"

using namespace sparta;
using namespace type_inference;

namespace {

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

// We abort the type checking process at the first error encountered.
class TypeCheckingException final : public std::runtime_error {
 public:
  explicit TypeCheckingException(const std::string& what_arg)
      : std::runtime_error(what_arg) {}
};

std::ostringstream& print_register(std::ostringstream& out, register_t reg) {
  if (reg == RESULT_REGISTER) {
    out << "result";
  } else {
    out << "register v" << reg;
  }
  return out;
}

void check_type_match(register_t reg, IRType actual, IRType expected) {
  if (actual == BOTTOM) {
    // There's nothing to do for unreachable code.
    return;
  }
  if (actual == SCALAR && expected != REFERENCE) {
    // If the type is SCALAR and we're checking compatibility with an integer
    // or float type, we just bail out.
    return;
  }
  if (!TypeDomain(actual).leq(TypeDomain(expected))) {
    std::ostringstream out;
    print_register(out, reg) << ": expected type " << expected << ", but found "
                             << actual << " instead";
    throw TypeCheckingException(out.str());
  }
}

void check_wide_type_match(register_t reg,
                           IRType actual1,
                           IRType actual2,
                           IRType expected1,
                           IRType expected2) {
  if (actual1 == BOTTOM) {
    // There's nothing to do for unreachable code.
    return;
  }

  if (actual1 == SCALAR1 && actual2 == SCALAR2) {
    // If type of the pair of registers is (SCALAR1, SCALAR2), we just bail
    // out.
    return;
  }
  if (!(TypeDomain(actual1).leq(TypeDomain(expected1)) &&
        TypeDomain(actual2).leq(TypeDomain(expected2)))) {
    std::ostringstream out;
    print_register(out, reg)
        << ": expected type (" << expected1 << ", " << expected2
        << "), but found (" << actual1 << ", " << actual2 << ") instead";
    throw TypeCheckingException(out.str());
  }
}

void assume_type(TypeEnvironment* state,
                 register_t reg,
                 IRType expected,
                 bool ignore_top = false) {
  if (state->is_bottom()) {
    // There's nothing to do for unreachable code.
    return;
  }
  IRType actual = state->get_type(reg).element();
  if (ignore_top && actual == TOP) {
    return;
  }
  check_type_match(reg, actual, /* expected */ expected);
}

void assume_wide_type(TypeEnvironment* state,
                      register_t reg,
                      IRType expected1,
                      IRType expected2) {
  if (state->is_bottom()) {
    // There's nothing to do for unreachable code.
    return;
  }
  IRType actual1 = state->get_type(reg).element();
  IRType actual2 = state->get_type(reg + 1).element();
  check_wide_type_match(reg,
                        actual1,
                        actual2,
                        /* expected1 */ expected1,
                        /* expected2 */ expected2);
}

// This is used for the operand of a comparison operation with zero. The
// complexity here is that this operation may be performed on either an
// integer or a reference.
void assume_comparable_with_zero(TypeEnvironment* state, register_t reg) {
  if (state->is_bottom()) {
    // There's nothing to do for unreachable code.
    return;
  }
  IRType t = state->get_type(reg).element();
  if (t == SCALAR) {
    // We can't say anything conclusive about a register that has SCALAR type,
    // so we just bail out.
    return;
  }
  if (!(TypeDomain(t).leq(TypeDomain(REFERENCE)) ||
        TypeDomain(t).leq(TypeDomain(INT)))) {
    std::ostringstream out;
    print_register(out, reg)
        << ": expected integer or reference type, but found " << t
        << " instead";
    throw TypeCheckingException(out.str());
  }
}

// This is used for the operands of a comparison operation between two
// registers. The complexity here is that this operation may be performed on
// either two integers or two references.
void assume_comparable(TypeEnvironment* state,
                       register_t reg1,
                       register_t reg2) {
  if (state->is_bottom()) {
    // There's nothing to do for unreachable code.
    return;
  }
  IRType t1 = state->get_type(reg1).element();
  IRType t2 = state->get_type(reg2).element();
  if (!((TypeDomain(t1).leq(TypeDomain(REFERENCE)) &&
         TypeDomain(t2).leq(TypeDomain(REFERENCE))) ||
        (TypeDomain(t1).leq(TypeDomain(SCALAR)) &&
         TypeDomain(t2).leq(TypeDomain(SCALAR)) && (t1 != FLOAT) &&
         (t2 != FLOAT)))) {
    // Two values can be used in a comparison operation if they either both
    // have the REFERENCE type or have non-float scalar types. Note that in
    // the case where one or both types have the SCALAR type, we can't
    // definitely rule out the absence of a type error.
    std::ostringstream out;
    print_register(out, reg1) << " and ";
    print_register(out, reg2)
        << ": incompatible types in comparison " << t1 << " and " << t2;
    throw TypeCheckingException(out.str());
  }
}

void assume_integer(TypeEnvironment* state, register_t reg) {
  assume_type(state, reg, /* expected */ INT);
}

void assume_float(TypeEnvironment* state, register_t reg) {
  assume_type(state, reg, /* expected */ FLOAT);
}

void assume_long(TypeEnvironment* state, register_t reg) {
  assume_wide_type(state, reg, /* expected1 */ LONG1, /* expected2 */ LONG2);
}

void assume_double(TypeEnvironment* state, register_t reg) {
  assume_wide_type(
      state, reg, /* expected1 */ DOUBLE1, /* expected2 */ DOUBLE2);
}

void assume_wide_scalar(TypeEnvironment* state, register_t reg) {
  assume_wide_type(
      state, reg, /* expected1 */ SCALAR1, /* expected2 */ SCALAR2);
}

class Result final {
 public:
  static Result Ok() { return Result(); }

  static Result make_error(const std::string& s) { return Result(s); }

  const std::string& error_message() const {
    always_assert(!is_ok);
    return m_error_message;
  }

  bool operator==(const Result& that) const {
    return is_ok == that.is_ok && m_error_message == that.m_error_message;
  }

  bool operator!=(const Result& that) const { return !(*this == that); }

 private:
  bool is_ok{true};
  std::string m_error_message;
  explicit Result(const std::string& s) : is_ok(false), m_error_message(s) {}
  Result() = default;
};

static bool has_move_result_pseudo(const MethodItemEntry& mie) {
  return mie.type == MFLOW_OPCODE && mie.insn->has_move_result_pseudo();
}

static bool is_move_result_pseudo(const MethodItemEntry& mie) {
  return mie.type == MFLOW_OPCODE &&
         opcode::is_move_result_pseudo(mie.insn->opcode());
}

/*
 * Do a linear pass to sanity-check the structure of the bytecode.
 */
Result check_structure(const DexMethod* method, bool check_no_overwrite_this) {
  check_no_overwrite_this &= !is_static(method);
  auto* code = method->get_code();
  IRInstruction* this_insn = nullptr;
  bool has_seen_non_load_param_opcode{false};
  for (auto it = code->begin(); it != code->end(); ++it) {
    // XXX we are using IRList::iterator instead of InstructionIterator here
    // because the latter does not support reverse iteration
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    auto* insn = it->insn;
    auto op = insn->opcode();

    if (has_seen_non_load_param_opcode && opcode::is_load_param(op)) {
      return Result::make_error("Encountered " + show(*it) +
                                " not at the start of the method");
    }
    has_seen_non_load_param_opcode = !opcode::is_load_param(op);

    if (check_no_overwrite_this) {
      if (op == IOPCODE_LOAD_PARAM_OBJECT && this_insn == nullptr) {
        this_insn = insn;
      } else if (insn->dests_size() && insn->dest() == this_insn->dest()) {
        return Result::make_error(
            "Encountered overwrite of `this` register by " + show(insn));
      }
    }

    // The instruction immediately before a move-result instruction must be
    // either an invoke-* or a filled-new-array instruction.
    if (is_move_result(op)) {
      auto prev = it;
      while (prev != code->begin()) {
        --prev;
        if (prev->type == MFLOW_OPCODE) {
          break;
        }
      }
      if (it == code->begin() || prev->type != MFLOW_OPCODE) {
        return Result::make_error("Encountered " + show(*it) +
                                  " at start of the method");
      }
      auto prev_op = prev->insn->opcode();
      if (!(is_invoke(prev_op) || is_filled_new_array(prev_op))) {
        return Result::make_error(
            "Encountered " + show(*it) +
            " without appropriate prefix "
            "instruction. Expected invoke or filled-new-array, got " +
            show(prev->insn));
      }
    } else if (opcode::is_move_result_pseudo(insn->opcode()) &&
               (it == code->begin() ||
                !has_move_result_pseudo(*std::prev(it)))) {
      return Result::make_error("Encountered " + show(*it) +
                                " without appropriate prefix "
                                "instruction");
    } else if (insn->has_move_result_pseudo() &&
               (it == code->end() || !is_move_result_pseudo(*std::next(it)))) {
      return Result::make_error("Did not find move-result-pseudo after " +
                                show(*it) + " in \n" + show(code));
    }
  }
  return Result::Ok();
}

} // namespace

IRTypeChecker::~IRTypeChecker() {}

IRTypeChecker::IRTypeChecker(DexMethod* dex_method)
    : m_dex_method(dex_method),
      m_complete(false),
      m_enable_polymorphic_constants(false),
      m_verify_moves(false),
      m_check_no_overwrite_this(false),
      m_good(true),
      m_what("OK") {}

void IRTypeChecker::run() {
  IRCode* code = m_dex_method->get_code();
  if (m_complete) {
    // The type checker can only be run once on any given method.
    return;
  }

  if (code == nullptr) {
    // If the method has no associated code, the type checking trivially
    // succeeds.
    m_complete = true;
    return;
  }

  auto result = check_structure(m_dex_method, m_check_no_overwrite_this);
  if (result != Result::Ok()) {
    m_complete = true;
    m_good = false;
    m_what = result.error_message();
    return;
  }

  // We then infer types for all the registers used in the method.
  code->build_cfg(/* editable */ false);
  const cfg::ControlFlowGraph& cfg = code->cfg();
  m_type_inference =
      std::make_unique<TypeInference>(cfg, m_enable_polymorphic_constants);
  m_type_inference->run(m_dex_method);

  // Finally, we use the inferred types to type-check each instruction in the
  // method. We stop at the first type error encountered.
  auto& type_envs = m_type_inference->get_type_environments();
  for (const MethodItemEntry& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    try {
      auto it = type_envs.find(insn);
      always_assert(it != type_envs.end());
      check_instruction(insn, &it->second);
    } catch (const TypeCheckingException& e) {
      m_good = false;
      std::ostringstream out;
      out << "Type error in method " << m_dex_method->get_deobfuscated_name()
          << " at instruction '" << SHOW(insn) << "' @ " << std::hex
          << static_cast<const void*>(&mie) << " for " << e.what();
      m_what = out.str();
      m_complete = true;
      return;
    }
  }
  m_complete = true;

  if (traceEnabled(TYPE, 5)) {
    std::ostringstream out;
    m_type_inference->print(out);
    TRACE(TYPE, 5, "%s\n", out.str().c_str());
  }
}

void IRTypeChecker::assume_scalar(TypeEnvironment* state,
                                  register_t reg,
                                  bool in_move) const {
  assume_type(state,
              reg,
              /* expected */ SCALAR,
              /* ignore_top */ in_move && !m_verify_moves);
}

void IRTypeChecker::assume_reference(TypeEnvironment* state,
                                     register_t reg,
                                     bool in_move) const {
  assume_type(state,
              reg,
              /* expected */ REFERENCE,
              /* ignore_top */ in_move && !m_verify_moves);
}

// This method performs type checking only: the type environment is not updated
// and the source registers of the instruction are checked against their
// expected types.
//
// Similarly, the various assume_* functions used throughout the code to check
// that the inferred type of a register matches with its expected type, as
// derived from the context.
void IRTypeChecker::check_instruction(IRInstruction* insn,
                                      TypeEnvironment* current_state) const {
  switch (insn->opcode()) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE: {
    // IOPCODE_LOAD_PARAM_* instructions have been processed before the
    // analysis.
    break;
  }
  case OPCODE_NOP: {
    break;
  }
  case OPCODE_MOVE: {
    assume_scalar(current_state, insn->src(0), /* in_move */ true);
    break;
  }
  case OPCODE_MOVE_OBJECT: {
    assume_reference(current_state, insn->src(0), /* in_move */ true);
    break;
  }
  case OPCODE_MOVE_WIDE: {
    assume_wide_scalar(current_state, insn->src(0));
    break;
  }
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case OPCODE_MOVE_RESULT: {
    assume_scalar(current_state, RESULT_REGISTER);
    break;
  }
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case OPCODE_MOVE_RESULT_OBJECT: {
    assume_reference(current_state, RESULT_REGISTER);
    break;
  }
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case OPCODE_MOVE_RESULT_WIDE: {
    assume_wide_scalar(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_MOVE_EXCEPTION: {
    // We don't know where to grab the type of the just-caught exception.
    // Simply set to j.l.Throwable here.
    break;
  }
  case OPCODE_RETURN_VOID: {
    break;
  }
  case OPCODE_RETURN: {
    assume_scalar(current_state, insn->src(0));
    break;
  }
  case OPCODE_RETURN_WIDE: {
    assume_wide_scalar(current_state, insn->src(0));
    break;
  }
  case OPCODE_RETURN_OBJECT: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_CONST: {
    break;
  }
  case OPCODE_CONST_WIDE: {
    break;
  }
  case OPCODE_CONST_STRING: {
    break;
  }
  case OPCODE_CONST_CLASS: {
    break;
  }
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_CHECK_CAST: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_INSTANCE_OF:
  case OPCODE_ARRAY_LENGTH: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_NEW_INSTANCE: {
    break;
  }
  case OPCODE_NEW_ARRAY: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_FILLED_NEW_ARRAY: {
    const DexType* element_type = get_array_type(insn->get_type());
    // We assume that structural constraints on the bytecode are satisfied,
    // i.e., the type is indeed an array type.
    always_assert(element_type != nullptr);
    bool is_array_of_references = is_object(element_type);
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      if (is_array_of_references) {
        assume_reference(current_state, insn->src(i));
      } else {
        assume_scalar(current_state, insn->src(i));
      }
    }
    break;
  }
  case OPCODE_FILL_ARRAY_DATA: {
    break;
  }
  case OPCODE_THROW: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_GOTO: {
    break;
  }
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT: {
    assume_float(current_state, insn->src(0));
    assume_float(current_state, insn->src(1));
    break;
  }
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE: {
    assume_double(current_state, insn->src(0));
    assume_double(current_state, insn->src(1));
    break;
  }
  case OPCODE_CMP_LONG: {
    assume_long(current_state, insn->src(0));
    assume_long(current_state, insn->src(1));
    break;
  }
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE: {
    assume_comparable(current_state, insn->src(0), insn->src(1));
    break;
  }
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE: {
    assume_integer(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ: {
    assume_comparable_with_zero(current_state, insn->src(0));
    break;
  }
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_AGET: {
    assume_reference(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT: {
    assume_reference(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_AGET_WIDE: {
    assume_reference(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_AGET_OBJECT: {
    assume_reference(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_APUT: {
    assume_scalar(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    assume_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT: {
    assume_integer(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    assume_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_APUT_WIDE: {
    assume_wide_scalar(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    assume_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_APUT_OBJECT: {
    assume_reference(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    assume_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_IGET: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_IGET_WIDE: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_IGET_OBJECT: {
    assume_reference(current_state, insn->src(0));
    always_assert(insn->has_field());
    break;
  }
  case OPCODE_IPUT: {
    const DexType* type = insn->get_field()->get_type();
    if (is_float(type)) {
      assume_float(current_state, insn->src(0));
    } else {
      assume_integer(current_state, insn->src(0));
    }
    assume_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT: {
    assume_integer(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_IPUT_WIDE: {
    assume_wide_scalar(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_IPUT_OBJECT: {
    assume_reference(current_state, insn->src(0));
    assume_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_SGET: {
    break;
  }
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT: {
    break;
  }
  case OPCODE_SGET_WIDE: {
    break;
  }
  case OPCODE_SGET_OBJECT: {
    break;
  }
  case OPCODE_SPUT: {
    const DexType* type = insn->get_field()->get_type();
    if (is_float(type)) {
      assume_float(current_state, insn->src(0));
    } else {
      assume_integer(current_state, insn->src(0));
    }
    break;
  }
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_SPUT_WIDE: {
    assume_wide_scalar(current_state, insn->src(0));
    break;
  }
  case OPCODE_SPUT_OBJECT: {
    assume_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    DexMethodRef* dex_method = insn->get_method();
    auto arg_types = dex_method->get_proto()->get_args()->get_type_list();
    size_t expected_args =
        (insn->opcode() != OPCODE_INVOKE_STATIC ? 1 : 0) + arg_types.size();
    if (insn->arg_word_count() != expected_args) {
      std::ostringstream out;
      out << SHOW(insn) << ": argument count mismatch; "
          << "expected " << expected_args << ", "
          << "but found " << insn->arg_word_count() << " instead";
      throw TypeCheckingException(out.str());
    }
    size_t src_idx{0};
    if (insn->opcode() != OPCODE_INVOKE_STATIC) {
      // The first argument is a reference to the object instance on which the
      // method is invoked.
      assume_reference(current_state, insn->src(src_idx++));
    }
    for (DexType* arg_type : arg_types) {
      if (is_object(arg_type)) {
        assume_reference(current_state, insn->src(src_idx++));
        continue;
      }
      if (is_integer(arg_type)) {
        assume_integer(current_state, insn->src(src_idx++));
        continue;
      }
      if (is_long(arg_type)) {
        assume_long(current_state, insn->src(src_idx++));
        continue;
      }
      if (is_float(arg_type)) {
        assume_float(current_state, insn->src(src_idx++));
        continue;
      }
      always_assert(is_double(arg_type));
      assume_double(current_state, insn->src(src_idx++));
    }
    break;
  }
  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG: {
    assume_long(current_state, insn->src(0));
    break;
  }
  case OPCODE_NEG_FLOAT: {
    assume_float(current_state, insn->src(0));
    break;
  }
  case OPCODE_NEG_DOUBLE: {
    assume_double(current_state, insn->src(0));
    break;
  }
  case OPCODE_INT_TO_BYTE:
  case OPCODE_INT_TO_CHAR:
  case OPCODE_INT_TO_SHORT: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_LONG_TO_INT: {
    assume_long(current_state, insn->src(0));
    break;
  }
  case OPCODE_FLOAT_TO_INT: {
    assume_float(current_state, insn->src(0));
    break;
  }
  case OPCODE_DOUBLE_TO_INT: {
    assume_double(current_state, insn->src(0));
    break;
  }
  case OPCODE_INT_TO_LONG: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_FLOAT_TO_LONG: {
    assume_float(current_state, insn->src(0));
    break;
  }
  case OPCODE_DOUBLE_TO_LONG: {
    assume_double(current_state, insn->src(0));
    break;
  }
  case OPCODE_INT_TO_FLOAT: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_LONG_TO_FLOAT: {
    assume_long(current_state, insn->src(0));
    break;
  }
  case OPCODE_DOUBLE_TO_FLOAT: {
    assume_double(current_state, insn->src(0));
    break;
  }
  case OPCODE_INT_TO_DOUBLE: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_LONG_TO_DOUBLE: {
    assume_long(current_state, insn->src(0));
    break;
  }
  case OPCODE_FLOAT_TO_DOUBLE: {
    assume_float(current_state, insn->src(0));
    break;
  }
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_SHL_INT:
  case OPCODE_SHR_INT:
  case OPCODE_USHR_INT: {
    assume_integer(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT: {
    assume_integer(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG: {
    assume_long(current_state, insn->src(0));
    assume_long(current_state, insn->src(1));
    break;
  }
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG: {
    assume_long(current_state, insn->src(0));
    assume_long(current_state, insn->src(1));
    break;
  }
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG: {
    assume_long(current_state, insn->src(0));
    assume_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT: {
    assume_float(current_state, insn->src(0));
    assume_float(current_state, insn->src(1));
    break;
  }
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE: {
    assume_double(current_state, insn->src(0));
    assume_double(current_state, insn->src(1));
    break;
  }
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_RSUB_INT:
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_AND_INT_LIT16:
  case OPCODE_OR_INT_LIT16:
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_ADD_INT_LIT8:
  case OPCODE_RSUB_INT_LIT8:
  case OPCODE_MUL_INT_LIT8:
  case OPCODE_AND_INT_LIT8:
  case OPCODE_OR_INT_LIT8:
  case OPCODE_XOR_INT_LIT8:
  case OPCODE_SHL_INT_LIT8:
  case OPCODE_SHR_INT_LIT8:
  case OPCODE_USHR_INT_LIT8: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8: {
    assume_integer(current_state, insn->src(0));
    break;
  }
  }
}

IRType IRTypeChecker::get_type(IRInstruction* insn, uint16_t reg) const {
  check_completion();
  auto& type_envs = m_type_inference->get_type_environments();
  auto it = type_envs.find(insn);
  if (it == type_envs.end()) {
    // The instruction doesn't belong to this method. We treat this as
    // unreachable code and return BOTTOM.
    return BOTTOM;
  }
  return it->second.get_type(reg).element();
}

const DexType* IRTypeChecker::get_dex_type(IRInstruction* insn,
                                           uint16_t reg) const {
  check_completion();
  auto& type_envs = m_type_inference->get_type_environments();
  auto it = type_envs.find(insn);
  if (it == type_envs.end()) {
    // The instruction doesn't belong to this method. We treat this as
    // unreachable code and return BOTTOM.
    return nullptr;
  }
  return *it->second.get_dex_type(reg);
}

std::ostream& operator<<(std::ostream& output, const IRTypeChecker& checker) {
  checker.m_type_inference->print(output);
  return output;
}
