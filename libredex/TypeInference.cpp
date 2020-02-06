/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeInference.h"

#include <ostream>
#include <sstream>

#include "DexTypeDomain.h"

std::ostream& operator<<(std::ostream& output, const IRType& type) {
  switch (type) {
  case BOTTOM: {
    output << "BOTTOM";
    break;
  }
  case ZERO: {
    output << "ZERO";
    break;
  }
  case CONST: {
    output << "CONST";
    break;
  }
  case CONST1: {
    output << "CONST1";
    break;
  }
  case CONST2: {
    output << "CONST2";
    break;
  }
  case REFERENCE: {
    output << "REFERENCE";
    break;
  }
  case INT: {
    output << "INT";
    break;
  }
  case FLOAT: {
    output << "FLOAT";
    break;
  }
  case LONG1: {
    output << "LONG1";
    break;
  }
  case LONG2: {
    output << "LONG2";
    break;
  }
  case DOUBLE1: {
    output << "DOUBLE1";
    break;
  }
  case DOUBLE2: {
    output << "DOUBLE2";
    break;
  }
  case SCALAR: {
    output << "SCALAR";
    break;
  }
  case SCALAR1: {
    output << "SCALAR1";
    break;
  }
  case SCALAR2: {
    output << "SCALAR2";
    break;
  }
  case TOP: {
    output << "TOP";
    break;
  }
  }
  return output;
}

namespace type_inference {

TypeLattice type_lattice(
    {BOTTOM, ZERO, CONST, CONST1, CONST2, REFERENCE, INT, FLOAT, LONG1, LONG2,
     DOUBLE1, DOUBLE2, SCALAR, SCALAR1, SCALAR2, TOP},
    {{BOTTOM, ZERO},    {BOTTOM, CONST1},   {BOTTOM, CONST2},
     {ZERO, REFERENCE}, {ZERO, CONST},      {CONST, INT},
     {CONST, FLOAT},    {CONST1, LONG1},    {CONST1, DOUBLE1},
     {CONST2, LONG2},   {CONST2, DOUBLE2},  {INT, SCALAR},
     {FLOAT, SCALAR},   {LONG1, SCALAR1},   {DOUBLE1, SCALAR1},
     {LONG2, SCALAR2},  {DOUBLE2, SCALAR2}, {REFERENCE, TOP},
     {SCALAR, TOP},     {SCALAR1, TOP},     {SCALAR2, TOP}});

void set_type(TypeEnvironment* state, reg_t reg, const TypeDomain& type) {
  state->set_type(reg, type);
}

void set_integer(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(INT));
}

void set_float(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(FLOAT));
}

void set_scalar(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(SCALAR));
}

void set_reference(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(REFERENCE));
}

void set_reference(TypeEnvironment* state,
                   reg_t reg,
                   const boost::optional<const DexType*>& dex_type_opt) {
  state->set_type(reg, TypeDomain(REFERENCE));
  const DexTypeDomain dex_type =
      dex_type_opt ? DexTypeDomain(*dex_type_opt) : DexTypeDomain::top();
  state->set_concrete_type(reg, dex_type);
}

void set_long(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(LONG1));
  state->set_type(reg + 1, TypeDomain(LONG2));
}

void set_double(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(DOUBLE1));
  state->set_type(reg + 1, TypeDomain(DOUBLE2));
}

void set_wide_scalar(TypeEnvironment* state, reg_t reg) {
  state->set_type(reg, TypeDomain(SCALAR1));
  state->set_type(reg + 1, TypeDomain(SCALAR2));
}

// This is used for the operand of a comparison operation with zero. The
// complexity here is that this operation may be performed on either an
// integer or a reference.
void refine_comparable_with_zero(TypeEnvironment* state, reg_t reg) {
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
    // The type is incompatible with the operation and hence, the code that
    // follows is unreachable.
    state->set_to_bottom();
    return;
  }
}

// This is used for the operands of a comparison operation between two
// registers. The complexity here is that this operation may be performed on
// either two integers or two references.
void refine_comparable(TypeEnvironment* state, reg_t reg1, reg_t reg2) {
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
    // The types are incompatible and hence, the code that follows is
    // unreachable.
    state->set_to_bottom();
    return;
  }
}

TypeDomain TypeInference::refine_type(const TypeDomain& type,
                                      IRType expected,
                                      IRType const_type,
                                      IRType scalar_type) const {
  auto refined_type = type.meet(TypeDomain(expected));
  // If constants are not considered polymorphic (the default behavior of the
  // Android verifier), we lift the constant to the type expected in the given
  // context. This only makes sense if the expected type is fully determined
  // by the context, i.e., is not a scalar type (SCALAR/SCALAR1/SCALAR2).
  if (type.leq(TypeDomain(const_type)) && expected != scalar_type) {
    return refined_type.is_bottom() ? refined_type : TypeDomain(expected);
  }
  return refined_type;
}

void TypeInference::refine_type(TypeEnvironment* state,
                                reg_t reg,
                                IRType expected) const {
  state->update_type(reg, [this, expected](const TypeDomain& type) {
    return refine_type(type, expected, /* const_type */ CONST,
                       /* scalar_type */ SCALAR);
  });
}

void TypeInference::refine_wide_type(TypeEnvironment* state,
                                     reg_t reg,
                                     IRType expected1,
                                     IRType expected2) const {
  state->update_type(reg, [this, expected1](const TypeDomain& type) {
    return refine_type(type,
                       expected1,
                       /* const_type */ CONST1,
                       /* scalar_type */ SCALAR1);
  });
  state->update_type(reg + 1, [this, expected2](const TypeDomain& type) {
    return refine_type(type,
                       expected2,
                       /* const_type */ CONST2,
                       /* scalar_type */ SCALAR2);
  });
}

void TypeInference::refine_reference(TypeEnvironment* state, reg_t reg) const {
  refine_type(state,
              reg,
              /* expected */ REFERENCE);
}

void TypeInference::refine_scalar(TypeEnvironment* state, reg_t reg) const {
  refine_type(state,
              reg,
              /* expected */ SCALAR);
}

void TypeInference::refine_integer(TypeEnvironment* state, reg_t reg) const {
  refine_type(state, reg, /* expected */ INT);
}

void TypeInference::refine_float(TypeEnvironment* state, reg_t reg) const {
  refine_type(state, reg, /* expected */ FLOAT);
}

void TypeInference::refine_wide_scalar(TypeEnvironment* state,
                                       reg_t reg) const {
  refine_wide_type(state, reg, /* expected1 */ SCALAR1,
                   /* expected2 */ SCALAR2);
}

void TypeInference::refine_long(TypeEnvironment* state, reg_t reg) const {
  refine_wide_type(state, reg, /* expected1 */ LONG1, /* expected2 */ LONG2);
}

void TypeInference::refine_double(TypeEnvironment* state, reg_t reg) const {
  refine_wide_type(state, reg, /* expected1 */ DOUBLE1,
                   /* expected2 */ DOUBLE2);
}

void TypeInference::run(const DexMethod* dex_method) {
  run(is_static(dex_method), dex_method->get_class(),
      dex_method->get_proto()->get_args());
}

void TypeInference::run(bool is_static,
                        DexType* declaring_type,
                        DexTypeList* args) {
  // We need to compute the initial environment by assigning the parameter
  // registers their correct types derived from the method's signature. The
  // IOPCODE_LOAD_PARAM_* instructions are pseudo-operations that are used to
  // specify the formal parameters of the method. They must be interpreted
  // separately.
  auto init_state = TypeEnvironment::top();
  const auto& signature = args->get_type_list();
  auto sig_it = signature.begin();
  bool first_param = true;
  // By construction, the IOPCODE_LOAD_PARAM_* instructions are located at the
  // beginning of the entry block of the CFG.
  for (const auto& mie : InstructionIterable(m_cfg.entry_block())) {
    IRInstruction* insn = mie.insn;
    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM_OBJECT: {
      if (first_param && !is_static) {
        // If the method is not static, the first parameter corresponds to
        // `this`.
        first_param = false;
        set_reference(&init_state, insn->dest(), declaring_type);
      } else {
        // This is a regular parameter of the method.
        const DexType* type = *sig_it;
        always_assert(sig_it++ != signature.end());
        set_reference(&init_state, insn->dest(), type);
      }
      break;
    }
    case IOPCODE_LOAD_PARAM: {
      always_assert(sig_it != signature.end());
      if (type::is_float(*sig_it++)) {
        set_float(&init_state, insn->dest());
      } else {
        set_integer(&init_state, insn->dest());
      }
      break;
    }
    case IOPCODE_LOAD_PARAM_WIDE: {
      always_assert(sig_it != signature.end());
      if (type::is_double(*sig_it++)) {
        set_double(&init_state, insn->dest());
      } else {
        set_long(&init_state, insn->dest());
      }
      break;
    }
    default: {
      // We've reached the end of the LOAD_PARAM_* instruction block and we
      // simply exit the loop. Note that premature loop exit is probably the
      // only legitimate use of goto in C++ code.
      goto done;
    }
    }
  }
done:
  MonotonicFixpointIterator::run(init_state);
  populate_type_environments();
}

// This method analyzes an instruction and updates the type environment
// accordingly during the fixpoint iteration.
//
// Similarly, the various refine_* functions are used to refine the
// type of a register depending on the context (e.g., from SCALAR to INT).
void TypeInference::analyze_instruction(const IRInstruction* insn,
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
    refine_scalar(current_state, insn->src(0));
    set_type(current_state, insn->dest(),
             current_state->get_type(insn->src(0)));
    break;
  }
  case OPCODE_MOVE_OBJECT: {
    refine_reference(current_state, insn->src(0));
    if (current_state->get_type(insn->src(0)) == TypeDomain(REFERENCE)) {
      const auto& dex_type_opt = current_state->get_dex_type(insn->src(0));
      set_reference(current_state, insn->dest(), dex_type_opt);
    } else {
      set_type(current_state, insn->dest(),
               current_state->get_type(insn->src(0)));
    }
    break;
  }
  case OPCODE_MOVE_WIDE: {
    refine_wide_scalar(current_state, insn->src(0));
    TypeDomain td1 = current_state->get_type(insn->src(0));
    TypeDomain td2 = current_state->get_type(insn->src(0) + 1);
    set_type(current_state, insn->dest(), td1);
    set_type(current_state, insn->dest() + 1, td2);
    break;
  }
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case OPCODE_MOVE_RESULT: {
    refine_scalar(current_state, RESULT_REGISTER);
    set_type(current_state, insn->dest(),
             current_state->get_type(RESULT_REGISTER));
    break;
  }
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case OPCODE_MOVE_RESULT_OBJECT: {
    refine_reference(current_state, RESULT_REGISTER);
    set_reference(current_state,
                  insn->dest(),
                  current_state->get_dex_type(RESULT_REGISTER));
    break;
  }
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case OPCODE_MOVE_RESULT_WIDE: {
    refine_wide_scalar(current_state, RESULT_REGISTER);
    set_type(current_state, insn->dest(),
             current_state->get_type(RESULT_REGISTER));
    set_type(current_state,
             insn->dest() + 1,
             current_state->get_type(RESULT_REGISTER + 1));
    break;
  }
  case OPCODE_MOVE_EXCEPTION: {
    // We don't know where to grab the type of the just-caught exception.
    // Simply set to j.l.Throwable here.
    set_reference(current_state, insn->dest(), type::java_lang_Throwable());
    break;
  }
  case OPCODE_RETURN_VOID: {
    break;
  }
  case OPCODE_RETURN: {
    refine_scalar(current_state, insn->src(0));
    break;
  }
  case OPCODE_RETURN_WIDE: {
    refine_wide_scalar(current_state, insn->src(0));
    break;
  }
  case OPCODE_RETURN_OBJECT: {
    refine_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_CONST: {
    if (insn->get_literal() == 0) {
      current_state->set_concrete_type(insn->dest(), DexTypeDomain::top());
      set_type(current_state, insn->dest(), TypeDomain(ZERO));
    } else {
      set_type(current_state, insn->dest(), TypeDomain(CONST));
    }
    break;
  }
  case OPCODE_CONST_WIDE: {
    set_type(current_state, insn->dest(), TypeDomain(CONST1));
    set_type(current_state, insn->dest() + 1, TypeDomain(CONST2));
    break;
  }
  case OPCODE_CONST_STRING: {
    set_reference(current_state, RESULT_REGISTER, type::java_lang_String());
    break;
  }
  case OPCODE_CONST_CLASS: {
    set_reference(current_state, RESULT_REGISTER, type::java_lang_Class());
    break;
  }
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT: {
    refine_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_CHECK_CAST: {
    refine_reference(current_state, insn->src(0));
    set_reference(current_state, RESULT_REGISTER, insn->get_type());
    break;
  }
  case OPCODE_INSTANCE_OF:
  case OPCODE_ARRAY_LENGTH: {
    refine_reference(current_state, insn->src(0));
    set_integer(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_NEW_INSTANCE: {
    set_reference(current_state, RESULT_REGISTER, insn->get_type());
    break;
  }
  case OPCODE_NEW_ARRAY: {
    refine_integer(current_state, insn->src(0));
    set_reference(current_state, RESULT_REGISTER, insn->get_type());
    break;
  }
  case OPCODE_FILLED_NEW_ARRAY: {
    const DexType* element_type =
        type::get_array_component_type(insn->get_type());
    // We assume that structural constraints on the bytecode are satisfied,
    // i.e., the type is indeed an array type.
    always_assert(element_type != nullptr);
    bool is_array_of_references = type::is_object(element_type);
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      if (is_array_of_references) {
        refine_reference(current_state, insn->src(i));
      } else {
        refine_scalar(current_state, insn->src(i));
      }
    }
    set_reference(current_state, RESULT_REGISTER, insn->get_type());
    break;
  }
  case OPCODE_FILL_ARRAY_DATA: {
    break;
  }
  case OPCODE_THROW: {
    refine_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_GOTO: {
    break;
  }
  case OPCODE_SWITCH: {
    refine_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT: {
    refine_float(current_state, insn->src(0));
    refine_float(current_state, insn->src(1));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE: {
    refine_double(current_state, insn->src(0));
    refine_double(current_state, insn->src(1));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_CMP_LONG: {
    refine_long(current_state, insn->src(0));
    refine_long(current_state, insn->src(1));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE: {
    refine_comparable(current_state, insn->src(0), insn->src(1));
    break;
  }
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE: {
    refine_integer(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    break;
  }
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ: {
    refine_comparable_with_zero(current_state, insn->src(0));
    break;
  }
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ: {
    refine_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_AGET: {
    refine_reference(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    set_scalar(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT: {
    refine_reference(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    set_integer(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_AGET_WIDE: {
    refine_reference(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    set_wide_scalar(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_AGET_OBJECT: {
    refine_reference(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    const auto dex_type_opt = current_state->get_dex_type(insn->src(0));
    if (dex_type_opt && *dex_type_opt && type::is_array(*dex_type_opt)) {
      const auto etype = type::get_array_component_type(*dex_type_opt);
      set_reference(current_state, RESULT_REGISTER, etype);
    } else {
      set_reference(current_state, RESULT_REGISTER);
    }
    break;
  }
  case OPCODE_APUT: {
    refine_scalar(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    refine_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT: {
    refine_integer(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    refine_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_APUT_WIDE: {
    refine_wide_scalar(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    refine_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_APUT_OBJECT: {
    refine_reference(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    refine_integer(current_state, insn->src(2));
    break;
  }
  case OPCODE_IGET: {
    refine_reference(current_state, insn->src(0));
    const DexType* type = insn->get_field()->get_type();
    if (type::is_float(type)) {
      set_float(current_state, RESULT_REGISTER);
    } else {
      set_integer(current_state, RESULT_REGISTER);
    }
    break;
  }
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT: {
    refine_reference(current_state, insn->src(0));
    set_integer(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_IGET_WIDE: {
    refine_reference(current_state, insn->src(0));
    const DexType* type = insn->get_field()->get_type();
    if (type::is_double(type)) {
      set_double(current_state, RESULT_REGISTER);
    } else {
      set_long(current_state, RESULT_REGISTER);
    }
    break;
  }
  case OPCODE_IGET_OBJECT: {
    refine_reference(current_state, insn->src(0));
    always_assert(insn->has_field());
    const auto field = insn->get_field();
    set_reference(current_state, RESULT_REGISTER, field->get_type());
    break;
  }
  case OPCODE_IPUT: {
    const DexType* type = insn->get_field()->get_type();
    if (type::is_float(type)) {
      refine_float(current_state, insn->src(0));
    } else {
      refine_integer(current_state, insn->src(0));
    }
    refine_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT: {
    refine_integer(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_IPUT_WIDE: {
    refine_wide_scalar(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_IPUT_OBJECT: {
    refine_reference(current_state, insn->src(0));
    refine_reference(current_state, insn->src(1));
    break;
  }
  case OPCODE_SGET: {
    DexType* type = insn->get_field()->get_type();
    if (type::is_float(type)) {
      set_float(current_state, RESULT_REGISTER);
    } else {
      set_integer(current_state, RESULT_REGISTER);
    }
    break;
  }
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT: {
    set_integer(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_SGET_WIDE: {
    DexType* type = insn->get_field()->get_type();
    if (type::is_double(type)) {
      set_double(current_state, RESULT_REGISTER);
    } else {
      set_long(current_state, RESULT_REGISTER);
    }
    break;
  }
  case OPCODE_SGET_OBJECT: {
    always_assert(insn->has_field());
    const auto field = insn->get_field();
    set_reference(current_state, RESULT_REGISTER, field->get_type());
    break;
  }
  case OPCODE_SPUT: {
    const DexType* type = insn->get_field()->get_type();
    if (type::is_float(type)) {
      refine_float(current_state, insn->src(0));
    } else {
      refine_integer(current_state, insn->src(0));
    }
    break;
  }
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT: {
    refine_integer(current_state, insn->src(0));
    break;
  }
  case OPCODE_SPUT_WIDE: {
    refine_wide_scalar(current_state, insn->src(0));
    break;
  }
  case OPCODE_SPUT_OBJECT: {
    refine_reference(current_state, insn->src(0));
    break;
  }
  case OPCODE_INVOKE_CUSTOM:
  case OPCODE_INVOKE_POLYMORPHIC: {
    // TODO(T59277083)
    always_assert_log(false,
                      "TypeInference::analyze_instruction does not support "
                      "invoke-custom and invoke-polymorphic yet");
    break;
  }
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    DexMethodRef* dex_method = insn->get_method();
    const auto& arg_types =
        dex_method->get_proto()->get_args()->get_type_list();
    size_t expected_args =
        (insn->opcode() != OPCODE_INVOKE_STATIC ? 1 : 0) + arg_types.size();
    always_assert(insn->srcs_size() == expected_args);

    size_t src_idx{0};
    if (insn->opcode() != OPCODE_INVOKE_STATIC) {
      // The first argument is a reference to the object instance on which the
      // method is invoked.
      refine_reference(current_state, insn->src(src_idx++));
    }
    for (DexType* arg_type : arg_types) {
      if (type::is_object(arg_type)) {
        refine_reference(current_state, insn->src(src_idx++));
        continue;
      }
      if (type::is_integer(arg_type)) {
        refine_integer(current_state, insn->src(src_idx++));
        continue;
      }
      if (type::is_long(arg_type)) {
        refine_long(current_state, insn->src(src_idx++));
        continue;
      }
      if (type::is_float(arg_type)) {
        refine_float(current_state, insn->src(src_idx++));
        continue;
      }
      always_assert(type::is_double(arg_type));
      refine_double(current_state, insn->src(src_idx++));
    }
    DexType* return_type = dex_method->get_proto()->get_rtype();
    if (type::is_void(return_type)) {
      break;
    }
    if (type::is_object(return_type)) {
      set_reference(current_state, RESULT_REGISTER, return_type);
      break;
    }
    if (type::is_integer(return_type)) {
      set_integer(current_state, RESULT_REGISTER);
      break;
    }
    if (type::is_long(return_type)) {
      set_long(current_state, RESULT_REGISTER);
      break;
    }
    if (type::is_float(return_type)) {
      set_float(current_state, RESULT_REGISTER);
      break;
    }
    always_assert(type::is_double(return_type));
    set_double(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT: {
    refine_integer(current_state, insn->src(0));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG: {
    refine_long(current_state, insn->src(0));
    set_long(current_state, insn->dest());
    break;
  }
  case OPCODE_NEG_FLOAT: {
    refine_float(current_state, insn->src(0));
    set_float(current_state, insn->dest());
    break;
  }
  case OPCODE_NEG_DOUBLE: {
    refine_double(current_state, insn->src(0));
    set_double(current_state, insn->dest());
    break;
  }
  case OPCODE_INT_TO_BYTE:
  case OPCODE_INT_TO_CHAR:
  case OPCODE_INT_TO_SHORT: {
    refine_integer(current_state, insn->src(0));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_LONG_TO_INT: {
    refine_long(current_state, insn->src(0));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_FLOAT_TO_INT: {
    refine_float(current_state, insn->src(0));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_DOUBLE_TO_INT: {
    refine_double(current_state, insn->src(0));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_INT_TO_LONG: {
    refine_integer(current_state, insn->src(0));
    set_long(current_state, insn->dest());
    break;
  }
  case OPCODE_FLOAT_TO_LONG: {
    refine_float(current_state, insn->src(0));
    set_long(current_state, insn->dest());
    break;
  }
  case OPCODE_DOUBLE_TO_LONG: {
    refine_double(current_state, insn->src(0));
    set_long(current_state, insn->dest());
    break;
  }
  case OPCODE_INT_TO_FLOAT: {
    refine_integer(current_state, insn->src(0));
    set_float(current_state, insn->dest());
    break;
  }
  case OPCODE_LONG_TO_FLOAT: {
    refine_long(current_state, insn->src(0));
    set_float(current_state, insn->dest());
    break;
  }
  case OPCODE_DOUBLE_TO_FLOAT: {
    refine_double(current_state, insn->src(0));
    set_float(current_state, insn->dest());
    break;
  }
  case OPCODE_INT_TO_DOUBLE: {
    refine_integer(current_state, insn->src(0));
    set_double(current_state, insn->dest());
    break;
  }
  case OPCODE_LONG_TO_DOUBLE: {
    refine_long(current_state, insn->src(0));
    set_double(current_state, insn->dest());
    break;
  }
  case OPCODE_FLOAT_TO_DOUBLE: {
    refine_float(current_state, insn->src(0));
    set_double(current_state, insn->dest());
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
    refine_integer(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT: {
    refine_integer(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    set_integer(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG: {
    refine_long(current_state, insn->src(0));
    refine_long(current_state, insn->src(1));
    set_long(current_state, insn->dest());
    break;
  }
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG: {
    refine_long(current_state, insn->src(0));
    refine_long(current_state, insn->src(1));
    set_long(current_state, RESULT_REGISTER);
    break;
  }
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG: {
    refine_long(current_state, insn->src(0));
    refine_integer(current_state, insn->src(1));
    set_long(current_state, insn->dest());
    break;
  }
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT: {
    refine_float(current_state, insn->src(0));
    refine_float(current_state, insn->src(1));
    set_float(current_state, insn->dest());
    break;
  }
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE: {
    refine_double(current_state, insn->src(0));
    refine_double(current_state, insn->src(1));
    set_double(current_state, insn->dest());
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
    refine_integer(current_state, insn->src(0));
    set_integer(current_state, insn->dest());
    break;
  }
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8: {
    refine_integer(current_state, insn->src(0));
    set_integer(current_state, RESULT_REGISTER);
    break;
  }
  }
}

void TypeInference::print(std::ostream& output) const {
  for (cfg::Block* block : m_cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      auto it = m_type_envs.find(insn);
      always_assert(it != m_type_envs.end());
      output << SHOW(insn) << " -- " << it->second << std::endl;
    }
  }
}

void TypeInference::traceState(TypeEnvironment* state) const {
  if (!traceEnabled(TYPE, 9)) {
    return;
  }
  std::ostringstream out;
  out << *state << std::endl;
  TRACE(TYPE, 9, "%s", out.str().c_str());
}

void TypeInference::populate_type_environments() {
  // We reserve enough space for the map in order to avoid repeated rehashing
  // during the computation.
  m_type_envs.reserve(m_cfg.blocks().size() * 16);
  for (cfg::Block* block : m_cfg.blocks()) {
    TypeEnvironment current_state = get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      m_type_envs.emplace(insn, current_state);
      analyze_instruction(insn, &current_state);
    }
  }
}

} // namespace type_inference
