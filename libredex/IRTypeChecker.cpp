/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IRTypeChecker.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "FiniteAbstractDomain.h"
#include "FixpointIterators.h"
#include "IRCode.h"
#include "Match.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "Show.h"

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

namespace irtc_impl {

using std::placeholders::_1;

using TypeLattice = BitVectorLattice<IRType, 16, std::hash<int>>;

TypeLattice type_lattice(
    {BOTTOM,
     ZERO,
     CONST,
     CONST1,
     CONST2,
     REFERENCE,
     INT,
     FLOAT,
     LONG1,
     LONG2,
     DOUBLE1,
     DOUBLE2,
     SCALAR,
     SCALAR1,
     SCALAR2,
     TOP},
    {{BOTTOM, ZERO},    {BOTTOM, CONST1},   {BOTTOM, CONST2},
     {ZERO, REFERENCE}, {ZERO, CONST},      {CONST, INT},
     {CONST, FLOAT},    {CONST1, LONG1},    {CONST1, DOUBLE1},
     {CONST2, LONG2},   {CONST2, DOUBLE2},  {INT, SCALAR},
     {FLOAT, SCALAR},   {LONG1, SCALAR1},   {DOUBLE1, SCALAR1},
     {LONG2, SCALAR2},  {DOUBLE2, SCALAR2}, {REFERENCE, TOP},
     {SCALAR, TOP},     {SCALAR1, TOP},     {SCALAR2, TOP}});

using TypeDomain = FiniteAbstractDomain<IRType,
                                        TypeLattice,
                                        TypeLattice::Encoding,
                                        &type_lattice>;

using register_t = uint32_t;

// We use this special register to denote the result of a method invocation or a
// filled-array creation. If the result is a wide value, RESULT_REGISTER + 1
// holds the second component of the result.
constexpr register_t RESULT_REGISTER =
    std::numeric_limits<register_t>::max() - 1;

using TypeEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, TypeDomain>;

// We abort the type checking process at the first error encountered.
class TypeCheckingException final : public std::runtime_error {
 public:
  explicit TypeCheckingException(const std::string& what_arg)
      : std::runtime_error(what_arg) {}
};

class TypeInference final
    : public MonotonicFixpointIterator<Block*, TypeEnvironment> {
 public:
  using NodeId = Block*;

  explicit TypeInference(const ControlFlowGraph& cfg, bool verify_moves)
      : MonotonicFixpointIterator(const_cast<Block*>(cfg.entry_block()),
                                  std::bind(&Block::succs, _1),
                                  std::bind(&Block::preds, _1),
                                  cfg.blocks().size()),
        m_cfg(cfg),
        m_verify_moves(verify_moves),
        m_inference(true) {}

  void run(DexMethod* dex_method) {
    // We need to compute the initial environment by assigning the parameter
    // registers their correct types derived from the method's signature. The
    // IOPCODE_LOAD_PARAM_* instructions are pseudo-operations that are used to
    // specify the formal parameters of the method. They must be interpreted
    // separately.
    auto init_state = TypeEnvironment::top();
    const auto& signature =
        dex_method->get_proto()->get_args()->get_type_list();
    auto sig_it = signature.begin();
    bool first_param = true;
    // By construction, the IOPCODE_LOAD_PARAM_* instructions are located at the
    // beginning of the entry block of the CFG.
    for (auto& mie : InstructionIterable(m_cfg.entry_block())) {
      IRInstruction* insn = mie.insn;
      switch (insn->opcode()) {
      case IOPCODE_LOAD_PARAM_OBJECT: {
        if (first_param && !is_static(dex_method)) {
          // If the method is not static, the first parameter corresponds to
          // `this`.
          first_param = false;
        } else {
          // This is a regular parameter of the method.
          always_assert(sig_it++ != signature.end());
        }
        set_reference(&init_state, insn->dest());
        break;
      }
      case IOPCODE_LOAD_PARAM: {
        always_assert(sig_it != signature.end());
        if (is_float(*sig_it++)) {
          set_float(&init_state, insn->dest());
        } else {
          set_integer(&init_state, insn->dest());
        }
        break;
      }
      case IOPCODE_LOAD_PARAM_WIDE: {
        always_assert(sig_it != signature.end());
        if (is_double(*sig_it++)) {
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
    // We turn off the type inference mode. All subsequent calls to
    // analyze_instruction will perform type checking.
    m_inference = false;
  }

  void analyze_node(const NodeId& node,
                    TypeEnvironment* current_state) const override {
    for (auto& mie : InstructionIterable(node)) {
      analyze_instruction(mie.insn, current_state);
    }
  }

  TypeEnvironment analyze_edge(
      const NodeId& /* source */,
      const NodeId& /* target */,
      const TypeEnvironment& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  // This method is used in two different modes. When m_inference == true, it
  // analyzes an instruction and updates the type environment accordingly. It is
  // used in this mode during the fixpoint iteration. Once a fixpoint has
  // been reached, m_inference is set to false and from this point on, the
  // method performs type checking only: the type environment is not updated and
  // the source registers of the instruction are checked against their expected
  // types.
  //
  // Similarly, the various assume_* functions used throughout the code operate
  // in two different modes. In type inference mode, they are used to refine the
  // type of a register depending on the context (e.g., from SCALAR to INT). In
  // type checking mode, they are used to check that the inferred type of a
  // register matches with its expected type, as derived from the context.
  void analyze_instruction(IRInstruction* insn,
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
    case OPCODE_MOVE:
    case OPCODE_MOVE_FROM16:
    case OPCODE_MOVE_16: {
      assume_scalar(current_state, insn->src(0), /* in_move */ true);
      set_type(current_state, insn->dest(), current_state->get(insn->src(0)));
      break;
    }
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_OBJECT_FROM16:
    case OPCODE_MOVE_OBJECT_16: {
      assume_reference(current_state, insn->src(0), /* in_move */ true);
      set_type(current_state, insn->dest(), current_state->get(insn->src(0)));
      break;
    }
    case OPCODE_MOVE_WIDE:
    case OPCODE_MOVE_WIDE_FROM16:
    case OPCODE_MOVE_WIDE_16: {
      assume_wide_scalar(current_state, insn->src(0));
      set_type(current_state, insn->dest(), current_state->get(insn->src(0)));
      set_type(current_state,
               insn->dest() + 1,
               current_state->get(insn->src(0) + 1));
      break;
    }
    case OPCODE_MOVE_RESULT: {
      assume_scalar(current_state, RESULT_REGISTER);
      set_type(
          current_state, insn->dest(), current_state->get(RESULT_REGISTER));
      break;
    }
    case OPCODE_MOVE_RESULT_OBJECT: {
      assume_reference(current_state, RESULT_REGISTER);
      set_type(
          current_state, insn->dest(), current_state->get(RESULT_REGISTER));
      break;
    }
    case OPCODE_MOVE_RESULT_WIDE: {
      assume_wide_scalar(current_state, RESULT_REGISTER);
      set_type(
          current_state, insn->dest(), current_state->get(RESULT_REGISTER));
      set_type(current_state,
               insn->dest() + 1,
               current_state->get(RESULT_REGISTER + 1));
      break;
    }
    case OPCODE_MOVE_EXCEPTION: {
      set_reference(current_state, insn->dest());
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
    case OPCODE_CONST_4:
    case OPCODE_CONST_16:
    case OPCODE_CONST:
    case OPCODE_CONST_HIGH16: {
      if (insn->literal() == 0) {
        set_type(current_state, insn->dest(), TypeDomain(ZERO));
      } else {
        set_type(current_state, insn->dest(), TypeDomain(CONST));
      }
      break;
    }
    case OPCODE_CONST_WIDE_16:
    case OPCODE_CONST_WIDE_32:
    case OPCODE_CONST_WIDE:
    case OPCODE_CONST_WIDE_HIGH16: {
      set_type(current_state, insn->dest(), TypeDomain(CONST1));
      set_type(current_state, insn->dest() + 1, TypeDomain(CONST2));
      break;
    }
    case OPCODE_CONST_STRING:
    case OPCODE_CONST_STRING_JUMBO:
    case OPCODE_CONST_CLASS: {
      set_reference(current_state, insn->dest());
      break;
    }
    case OPCODE_MONITOR_ENTER:
    case OPCODE_MONITOR_EXIT: {
      assume_reference(current_state, insn->src(0));
      break;
    }
    case OPCODE_CHECK_CAST: {
      assume_reference(current_state, insn->src(0));
      set_reference(current_state, insn->dest());
      break;
    }
    case OPCODE_INSTANCE_OF:
    case OPCODE_ARRAY_LENGTH: {
      assume_reference(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_NEW_INSTANCE: {
      set_reference(current_state, insn->dest());
      break;
    }
    case OPCODE_NEW_ARRAY: {
      assume_integer(current_state, insn->src(0));
      set_reference(current_state, insn->dest());
      break;
    }
    case OPCODE_FILLED_NEW_ARRAY:
    case OPCODE_FILLED_NEW_ARRAY_RANGE: {
      const DexType* type = get_array_type(insn->get_type());
      // We assume that structural constraints on the bytecode are satisfied,
      // i.e., the type is indeed an array type.
      always_assert(type != nullptr);
      // Although the Dalvik bytecode specification states that a
      // filled-new-array operation could be used with an array of references,
      // the Dex compiler seems to never generate that case. The assert is used
      // here as a safeguard.
      always_assert_log(
          !is_object(type), "Unexpected instruction '%s'", SHOW(insn));
      IRSourceIterator src_it(insn);
      while (!src_it.empty()) {
        assume_scalar(current_state, src_it.get_register());
      }
      set_reference(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_FILL_ARRAY_DATA: {
      break;
    }
    case OPCODE_THROW: {
      assume_reference(current_state, insn->src(0));
      break;
    }
    case OPCODE_GOTO:
    case OPCODE_GOTO_16:
    case OPCODE_GOTO_32: {
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
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_CMPL_DOUBLE:
    case OPCODE_CMPG_DOUBLE: {
      assume_double(current_state, insn->src(0));
      assume_double(current_state, insn->src(1));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_CMP_LONG: {
      assume_long(current_state, insn->src(0));
      assume_long(current_state, insn->src(1));
      set_integer(current_state, insn->dest());
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
      set_scalar(current_state, insn->dest());
      break;
    }
    case OPCODE_AGET_BOOLEAN:
    case OPCODE_AGET_BYTE:
    case OPCODE_AGET_CHAR:
    case OPCODE_AGET_SHORT: {
      assume_reference(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_AGET_WIDE: {
      assume_reference(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_wide_scalar(current_state, insn->dest());
      break;
    }
    case OPCODE_AGET_OBJECT: {
      assume_reference(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_reference(current_state, insn->dest());
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
      const DexType* type = insn->get_field()->get_type();
      if (is_float(type)) {
        set_float(current_state, insn->dest());
      } else {
        set_integer(current_state, insn->dest());
      }
      break;
    }
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_SHORT: {
      assume_reference(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_IGET_WIDE: {
      assume_reference(current_state, insn->src(0));
      const DexType* type = insn->get_field()->get_type();
      if (is_double(type)) {
        set_double(current_state, insn->dest());
      } else {
        set_long(current_state, insn->dest());
      }
      break;
    }
    case OPCODE_IGET_OBJECT: {
      assume_reference(current_state, insn->src(0));
      set_reference(current_state, insn->dest());
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
      DexType* type = insn->get_field()->get_type();
      if (is_float(type)) {
        set_float(current_state, insn->dest());
      } else {
        set_integer(current_state, insn->dest());
      }
      break;
    }
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_SHORT: {
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_SGET_WIDE: {
      DexType* type = insn->get_field()->get_type();
      if (is_double(type)) {
        set_double(current_state, insn->dest());
      } else {
        set_long(current_state, insn->dest());
      }
      break;
    }
    case OPCODE_SGET_OBJECT: {
      set_reference(current_state, insn->dest());
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
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_VIRTUAL_RANGE:
    case OPCODE_INVOKE_SUPER_RANGE:
    case OPCODE_INVOKE_DIRECT_RANGE:
    case OPCODE_INVOKE_STATIC_RANGE:
    case OPCODE_INVOKE_INTERFACE_RANGE: {
      DexMethodRef* dex_method = insn->get_method();
      auto arg_types = dex_method->get_proto()->get_args()->get_type_list();
      IRSourceIterator src_it(insn);
      if (!(insn->opcode() == OPCODE_INVOKE_STATIC ||
            insn->opcode() == OPCODE_INVOKE_STATIC_RANGE)) {
        // The first argument is a reference to the object instance on which the
        // method is invoked.
        assume_reference(current_state, src_it.get_register());
      }
      for (DexType* arg_type : arg_types) {
        if (is_object(arg_type)) {
          assume_reference(current_state, src_it.get_register());
          continue;
        }
        if (is_integer(arg_type)) {
          assume_integer(current_state, src_it.get_register());
          continue;
        }
        if (is_long(arg_type)) {
          assume_long(current_state, src_it.get_wide_register());
          continue;
        }
        if (is_float(arg_type)) {
          assume_float(current_state, src_it.get_register());
          continue;
        }
        always_assert(is_double(arg_type));
        assume_double(current_state, src_it.get_wide_register());
      }
      DexType* return_type = dex_method->get_proto()->get_rtype();
      if (is_void(return_type)) {
        break;
      }
      if (is_object(return_type)) {
        set_reference(current_state, RESULT_REGISTER);
        break;
      }
      if (is_integer(return_type)) {
        set_integer(current_state, RESULT_REGISTER);
        break;
      }
      if (is_long(return_type)) {
        set_long(current_state, RESULT_REGISTER);
        break;
      }
      if (is_float(return_type)) {
        set_float(current_state, RESULT_REGISTER);
        break;
      }
      always_assert(is_double(return_type));
      set_double(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_NEG_INT:
    case OPCODE_NOT_INT: {
      assume_integer(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_NEG_LONG:
    case OPCODE_NOT_LONG: {
      assume_long(current_state, insn->src(0));
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_NEG_FLOAT: {
      assume_float(current_state, insn->src(0));
      set_float(current_state, insn->dest());
      break;
    }
    case OPCODE_NEG_DOUBLE: {
      assume_double(current_state, insn->src(0));
      set_double(current_state, insn->dest());
      break;
    }
    case OPCODE_INT_TO_BYTE:
    case OPCODE_INT_TO_CHAR:
    case OPCODE_INT_TO_SHORT: {
      assume_integer(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_LONG_TO_INT: {
      assume_long(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_FLOAT_TO_INT: {
      assume_float(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_DOUBLE_TO_INT: {
      assume_double(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_INT_TO_LONG: {
      assume_integer(current_state, insn->src(0));
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_FLOAT_TO_LONG: {
      assume_float(current_state, insn->src(0));
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_DOUBLE_TO_LONG: {
      assume_double(current_state, insn->src(0));
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_INT_TO_FLOAT: {
      assume_integer(current_state, insn->src(0));
      set_float(current_state, insn->dest());
      break;
    }
    case OPCODE_LONG_TO_FLOAT: {
      assume_long(current_state, insn->src(0));
      set_float(current_state, insn->dest());
      break;
    }
    case OPCODE_DOUBLE_TO_FLOAT: {
      assume_double(current_state, insn->src(0));
      set_float(current_state, insn->dest());
      break;
    }
    case OPCODE_INT_TO_DOUBLE: {
      assume_integer(current_state, insn->src(0));
      set_double(current_state, insn->dest());
      break;
    }
    case OPCODE_LONG_TO_DOUBLE: {
      assume_long(current_state, insn->src(0));
      set_double(current_state, insn->dest());
      break;
    }
    case OPCODE_FLOAT_TO_DOUBLE: {
      assume_float(current_state, insn->src(0));
      set_double(current_state, insn->dest());
      break;
    }
    case OPCODE_ADD_INT:
    case OPCODE_SUB_INT:
    case OPCODE_MUL_INT:
    case OPCODE_DIV_INT:
    case OPCODE_REM_INT:
    case OPCODE_AND_INT:
    case OPCODE_OR_INT:
    case OPCODE_XOR_INT:
    case OPCODE_SHL_INT:
    case OPCODE_SHR_INT:
    case OPCODE_USHR_INT: {
      assume_integer(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_ADD_LONG:
    case OPCODE_SUB_LONG:
    case OPCODE_MUL_LONG:
    case OPCODE_DIV_LONG:
    case OPCODE_REM_LONG:
    case OPCODE_AND_LONG:
    case OPCODE_OR_LONG:
    case OPCODE_XOR_LONG: {
      assume_long(current_state, insn->src(0));
      assume_long(current_state, insn->src(1));
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_SHL_LONG:
    case OPCODE_SHR_LONG:
    case OPCODE_USHR_LONG: {
      assume_long(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_ADD_FLOAT:
    case OPCODE_SUB_FLOAT:
    case OPCODE_MUL_FLOAT:
    case OPCODE_DIV_FLOAT:
    case OPCODE_REM_FLOAT: {
      assume_float(current_state, insn->src(0));
      assume_float(current_state, insn->src(1));
      set_float(current_state, insn->dest());
      break;
    }
    case OPCODE_ADD_DOUBLE:
    case OPCODE_SUB_DOUBLE:
    case OPCODE_MUL_DOUBLE:
    case OPCODE_DIV_DOUBLE:
    case OPCODE_REM_DOUBLE: {
      assume_double(current_state, insn->src(0));
      assume_double(current_state, insn->src(1));
      set_double(current_state, insn->dest());
      break;
    }
    // In 2addr instructions, the destination and the source registers are
    // identical. Hence, there's no need to update the type of the destination
    // register.
    case OPCODE_ADD_INT_2ADDR:
    case OPCODE_SUB_INT_2ADDR:
    case OPCODE_MUL_INT_2ADDR:
    case OPCODE_DIV_INT_2ADDR:
    case OPCODE_REM_INT_2ADDR:
    case OPCODE_AND_INT_2ADDR:
    case OPCODE_OR_INT_2ADDR:
    case OPCODE_XOR_INT_2ADDR:
    case OPCODE_SHL_INT_2ADDR:
    case OPCODE_SHR_INT_2ADDR:
    case OPCODE_USHR_INT_2ADDR: {
      assume_integer(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      break;
    }
    case OPCODE_ADD_LONG_2ADDR:
    case OPCODE_SUB_LONG_2ADDR:
    case OPCODE_MUL_LONG_2ADDR:
    case OPCODE_DIV_LONG_2ADDR:
    case OPCODE_REM_LONG_2ADDR:
    case OPCODE_AND_LONG_2ADDR:
    case OPCODE_OR_LONG_2ADDR:
    case OPCODE_XOR_LONG_2ADDR: {
      assume_long(current_state, insn->src(0));
      assume_long(current_state, insn->src(1));
      break;
    }
    case OPCODE_SHL_LONG_2ADDR:
    case OPCODE_SHR_LONG_2ADDR:
    case OPCODE_USHR_LONG_2ADDR: {
      assume_long(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      break;
    }
    case OPCODE_ADD_FLOAT_2ADDR:
    case OPCODE_SUB_FLOAT_2ADDR:
    case OPCODE_MUL_FLOAT_2ADDR:
    case OPCODE_DIV_FLOAT_2ADDR:
    case OPCODE_REM_FLOAT_2ADDR: {
      assume_float(current_state, insn->src(0));
      assume_float(current_state, insn->src(1));
      break;
    }
    case OPCODE_ADD_DOUBLE_2ADDR:
    case OPCODE_SUB_DOUBLE_2ADDR:
    case OPCODE_MUL_DOUBLE_2ADDR:
    case OPCODE_DIV_DOUBLE_2ADDR:
    case OPCODE_REM_DOUBLE_2ADDR: {
      assume_double(current_state, insn->src(0));
      assume_double(current_state, insn->src(1));
      break;
    }
    case OPCODE_ADD_INT_LIT16:
    case OPCODE_RSUB_INT:
    case OPCODE_MUL_INT_LIT16:
    case OPCODE_DIV_INT_LIT16:
    case OPCODE_REM_INT_LIT16:
    case OPCODE_AND_INT_LIT16:
    case OPCODE_OR_INT_LIT16:
    case OPCODE_XOR_INT_LIT16:
    case OPCODE_ADD_INT_LIT8:
    case OPCODE_RSUB_INT_LIT8:
    case OPCODE_MUL_INT_LIT8:
    case OPCODE_DIV_INT_LIT8:
    case OPCODE_REM_INT_LIT8:
    case OPCODE_AND_INT_LIT8:
    case OPCODE_OR_INT_LIT8:
    case OPCODE_XOR_INT_LIT8:
    case OPCODE_SHL_INT_LIT8:
    case OPCODE_SHR_INT_LIT8:
    case OPCODE_USHR_INT_LIT8: {
      assume_integer(current_state, insn->src(0));
      set_integer(current_state, insn->dest());
      break;
    }
    case FOPCODE_PACKED_SWITCH:
    case FOPCODE_SPARSE_SWITCH:
    case FOPCODE_FILLED_ARRAY: {
      // Pseudo-opcodes have been simplified away by the IR.
      always_assert_log(false, "Unexpected instruction: %s", SHOW(insn));
    }
    }
  }

  void print(std::ostream& output) const {
    for (Block* block : m_cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        auto it = m_type_envs.find(insn);
        always_assert(it != m_type_envs.end());
        output << SHOW(insn) << " -- " << it->second << std::endl;
      }
    }
  }

 private:
  void populate_type_environments() {
    // We reserve enough space for the map in order to avoid repeated rehashing
    // during the computation.
    m_type_envs.reserve(m_cfg.blocks().size() * 16);
    for (Block* block : m_cfg.blocks()) {
      TypeEnvironment current_state = get_entry_state_at(block);
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        m_type_envs.emplace(insn, current_state);
        analyze_instruction(insn, &current_state);
      }
    }
  }

  void set_type(TypeEnvironment* state,
                register_t reg,
                const TypeDomain& type) const {
    if (m_inference) {
      state->set(reg, type);
    }
  }

  void set_integer(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(INT));
    }
  }

  void set_float(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(FLOAT));
    }
  }

  void set_scalar(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(SCALAR));
    }
  }

  void set_reference(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(REFERENCE));
    }
  }

  void set_long(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(LONG1));
      state->set(reg + 1, TypeDomain(LONG2));
    }
  }

  void set_double(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(DOUBLE1));
      state->set(reg + 1, TypeDomain(DOUBLE2));
    }
  }

  void set_wide_scalar(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set(reg, TypeDomain(SCALAR1));
      state->set(reg + 1, TypeDomain(SCALAR2));
    }
  }

  void assume_type(TypeEnvironment* state,
                   register_t reg,
                   IRType expected,
                   bool ignore_top = false) const {
    if (m_inference) {
      state->update(reg, [expected](const TypeDomain& type) {
        return type.meet(TypeDomain(expected));
      });
    } else {
      if (state->is_bottom()) {
        // There's nothing to do for unreachable code.
        return;
      }
      IRType actual = state->get(reg).element();
      if (ignore_top && actual == TOP) {
        return;
      }
      check_type_match(reg, actual, /* expected */ expected);
    }
  }

  void assume_wide_type(TypeEnvironment* state,
                        register_t reg,
                        IRType expected1,
                        IRType expected2) const {
    if (m_inference) {
      state->update(reg, [expected1](const TypeDomain& type) {
        return type.meet(TypeDomain(expected1));
      });
      state->update(reg + 1, [expected2](const TypeDomain& type) {
        return type.meet(TypeDomain(expected2));
      });
    } else {
      if (state->is_bottom()) {
        // There's nothing to do for unreachable code.
        return;
      }
      IRType actual1 = state->get(reg).element();
      IRType actual2 = state->get(reg + 1).element();
      check_wide_type_match(reg,
                            actual1,
                            actual2,
                            /* expected1 */ expected1,
                            /* expected2 */ expected2);
    }
  }

  void assume_reference(TypeEnvironment* state,
                        register_t reg,
                        bool in_move = false) const {
    assume_type(state,
                reg,
                /* expected */ REFERENCE,
                /* ignore_top */ in_move && !m_verify_moves);
  }

  void assume_scalar(TypeEnvironment* state,
                     register_t reg,
                     bool in_move = false) const {
    assume_type(state,
                reg,
                /* expected */ SCALAR,
                /* ignore_top */ in_move && !m_verify_moves);
  }

  void assume_integer(TypeEnvironment* state, register_t reg) const {
    assume_type(state, reg, /* expected */ INT);
  }

  void assume_float(TypeEnvironment* state, register_t reg) const {
    assume_type(state, reg, /* expected */ FLOAT);
  }

  void assume_wide_scalar(TypeEnvironment* state, register_t reg) const {
    assume_wide_type(
        state, reg, /* expected1 */ SCALAR1, /* expected2 */ SCALAR2);
  }

  void assume_long(TypeEnvironment* state, register_t reg) const {
    assume_wide_type(state, reg, /* expected1 */ LONG1, /* expected2 */ LONG2);
  }

  void assume_double(TypeEnvironment* state, register_t reg) const {
    assume_wide_type(
        state, reg, /* expected1 */ DOUBLE1, /* expected2 */ DOUBLE2);
  }

  // This is used for the operand of a comparison operation with zero. The
  // complexity here is that this operation may be performed on either an
  // integer or a reference.
  void assume_comparable_with_zero(TypeEnvironment* state,
                                   register_t reg) const {
    if (state->is_bottom()) {
      // There's nothing to do for unreachable code.
      return;
    }
    IRType t = state->get(reg).element();
    if (t == SCALAR) {
      // We can't say anything conclusive about a register that has SCALAR type,
      // so we just bail out.
      return;
    }
    if (!(TypeDomain(t).leq(TypeDomain(REFERENCE)) ||
          TypeDomain(t).leq(TypeDomain(INT)))) {
      if (m_inference) {
        // The type is incompatible with the operation and hence, the code that
        // follows is unreachable.
        state->set_to_bottom();
        return;
      }
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
                         register_t reg2) const {
    if (state->is_bottom()) {
      // There's nothing to do for unreachable code.
      return;
    }
    IRType t1 = state->get(reg1).element();
    IRType t2 = state->get(reg2).element();
    if (!((TypeDomain(t1).leq(TypeDomain(REFERENCE)) &&
           TypeDomain(t2).leq(TypeDomain(REFERENCE))) ||
          (TypeDomain(t1).leq(TypeDomain(SCALAR)) &&
           TypeDomain(t2).leq(TypeDomain(SCALAR)) && (t1 != FLOAT) &&
           (t2 != FLOAT)))) {
      // Two values can be used in a comparison operation if they either both
      // have the REFERENCE type or have non-float scalar types. Note that in
      // the case where one or both types have the SCALAR type, we can't
      // definitely rule out the absence of a type error.
      if (m_inference) {
        // The types are incompatible and hence, the code that follows is
        // unreachable.
        state->set_to_bottom();
        return;
      }
      std::ostringstream out;
      print_register(out, reg1) << " and ";
      print_register(out, reg2) << ": incompatible types in comparison " << t1
                                << " and " << t2;
      throw TypeCheckingException(out.str());
    }
  }

  void check_type_match(register_t reg, IRType actual, IRType expected) const {
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
      print_register(out, reg) << ": expected type " << expected
                               << ", but found " << actual << " instead";
      throw TypeCheckingException(out.str());
    }
  }

  void check_wide_type_match(register_t reg,
                             IRType actual1,
                             IRType actual2,
                             IRType expected1,
                             IRType expected2) const {
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
      print_register(out, reg) << ": expected type (" << expected1 << ", "
                               << expected2 << "), but found (" << actual1
                               << ", " << actual2 << ") instead";
      throw TypeCheckingException(out.str());
    }
  }

  std::ostringstream& print_register(std::ostringstream& out,
                                     register_t reg) const {
    if (reg == RESULT_REGISTER) {
      out << "result";
    } else {
      out << "register v" << reg;
    }
    return out;
  }

  const ControlFlowGraph& m_cfg;
  bool m_verify_moves;
  bool m_inference;
  std::unordered_map<IRInstruction*, TypeEnvironment> m_type_envs;

  friend class ::IRTypeChecker;
};

} // namespace irtc_impl

IRTypeChecker::~IRTypeChecker() {}

IRTypeChecker::IRTypeChecker(DexMethod* dex_method, bool verify_moves)
    : m_dex_method(dex_method), m_good(true), m_what("OK") {
  IRCode* code = dex_method->get_code();
  if (code == nullptr) {
    // If the method has no associated code, the type checking trivially
    // succeeds.
    return;
  }

  // We then infer types for all the registers used in the method.
  code->build_cfg();
  const ControlFlowGraph& cfg = code->cfg();
  m_type_inference =
      std::make_unique<irtc_impl::TypeInference>(cfg, verify_moves);
  m_type_inference->run(dex_method);

  // Finally, we use the inferred types to type-check each instruction in the
  // method. We stop at the first type error encountered.
  auto& type_envs = m_type_inference->m_type_envs;
  for (const MethodItemEntry& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    try {
      auto it = type_envs.find(insn);
      always_assert(it != type_envs.end());
      m_type_inference->analyze_instruction(insn, &it->second);
    } catch (const irtc_impl::TypeCheckingException& e) {
      m_good = false;
      std::ostringstream out;
      out << "Type error in method " << m_dex_method->get_deobfuscated_name()
          << " at instruction '" << SHOW(insn) << "' for " << e.what();
      m_what = out.str();
      return;
    }
  }
}

IRType IRTypeChecker::get_type(IRInstruction* insn, uint16_t reg) const {
  auto& type_envs = m_type_inference->m_type_envs;
  auto it = type_envs.find(insn);
  if (it == type_envs.end()) {
    // The instruction doesn't belong to this method. We treat this as
    // unreachable code and return BOTTOM.
    return BOTTOM;
  }
  return it->second.get(reg).element();
}

std::ostream& operator<<(std::ostream& output, const IRTypeChecker& checker) {
  checker.m_type_inference->print(output);
  return output;
}
