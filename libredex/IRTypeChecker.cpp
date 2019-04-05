/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRTypeChecker.h"

#include <boost/optional/optional_io.hpp>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "Debug.h"
#include "DexAccess.h"
#include "DexClass.h"
#include "DexTypeDomain.h"
#include "DexUtil.h"
#include "FiniteAbstractDomain.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "Match.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"
#include "Show.h"

using namespace sparta;

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
    {BOTTOM, ZERO, CONST, CONST1, CONST2, REFERENCE, INT, FLOAT, LONG1, LONG2,
     DOUBLE1, DOUBLE2, SCALAR, SCALAR1, SCALAR2, TOP},
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

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

using BasicTypeEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, TypeDomain>;

using DexTypeEnvironment =
    PatriciaTreeMapAbstractEnvironment<register_t, DexTypeDomain>;

class TypeEnvironment final
    : public ReducedProductAbstractDomain<TypeEnvironment,
                                          BasicTypeEnvironment,
                                          DexTypeEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      std::tuple<BasicTypeEnvironment, DexTypeEnvironment>& /* product */) {}

  TypeDomain get_type(register_t reg) const { return get<0>().get(reg); }

  void set_type(register_t reg, const TypeDomain type) {
    apply<0>([=](auto env) { env->set(reg, type); }, true);
  }

  void update_type(
      register_t reg,
      const std::function<TypeDomain(const TypeDomain&)>& operation) {
    apply<0>([=](auto env) { env->update(reg, operation); }, true);
  }

  boost::optional<const DexType*> get_dex_type(register_t reg) const {
    return get<1>().get(reg).get_dex_type();
  }

  void set_concrete_type(register_t reg, const DexTypeDomain& dex_type) {
    apply<1>([=](auto env) { env->set(reg, dex_type); }, true);
  }
};

// We abort the type checking process at the first error encountered.
class TypeCheckingException final : public std::runtime_error {
 public:
  explicit TypeCheckingException(const std::string& what_arg)
      : std::runtime_error(what_arg) {}
};

class TypeInference final : public BaseIRAnalyzer<TypeEnvironment> {
 public:

  TypeInference(const cfg::ControlFlowGraph& cfg,
                bool enable_polymorphic_constants,
                bool verify_moves)
      : BaseIRAnalyzer<TypeEnvironment>(cfg),
        m_cfg(cfg),
        m_enable_polymorphic_constants(enable_polymorphic_constants),
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
    for (const auto& mie : InstructionIterable(m_cfg.entry_block())) {
      IRInstruction* insn = mie.insn;
      switch (insn->opcode()) {
      case IOPCODE_LOAD_PARAM_OBJECT: {
        if (first_param && !is_static(dex_method)) {
          // If the method is not static, the first parameter corresponds to
          // `this`.
          first_param = false;
          set_reference(&init_state, insn->dest(), dex_method->get_class());
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
                           TypeEnvironment* current_state) const override {
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
      set_type(
          current_state, insn->dest(), current_state->get_type(insn->src(0)));
      break;
    }
    case OPCODE_MOVE_OBJECT: {
      assume_reference(current_state, insn->src(0), /* in_move */ true);
      if (current_state->get_type(insn->src(0)) == TypeDomain(REFERENCE)) {
        const auto& dex_type_opt = current_state->get_dex_type(insn->src(0));
        set_reference(current_state, insn->dest(), dex_type_opt);
      } else {
        set_type(
            current_state, insn->dest(), current_state->get_type(insn->src(0)));
      }
      break;
    }
    case OPCODE_MOVE_WIDE: {
      assume_wide_scalar(current_state, insn->src(0));
      TypeDomain td1 = current_state->get_type(insn->src(0));
      TypeDomain td2 = current_state->get_type(insn->src(0) + 1);
      set_type(current_state, insn->dest(), td1);
      set_type(current_state, insn->dest() + 1, td2);
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO:
    case OPCODE_MOVE_RESULT: {
      assume_scalar(current_state, RESULT_REGISTER);
      set_type(current_state,
               insn->dest(),
               current_state->get_type(RESULT_REGISTER));
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT: {
      assume_reference(current_state, RESULT_REGISTER);
      set_reference(current_state,
                    insn->dest(),
                    current_state->get_dex_type(RESULT_REGISTER));
      break;
    }
    case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    case OPCODE_MOVE_RESULT_WIDE: {
      assume_wide_scalar(current_state, RESULT_REGISTER);
      set_type(current_state,
               insn->dest(),
               current_state->get_type(RESULT_REGISTER));
      set_type(current_state,
               insn->dest() + 1,
               current_state->get_type(RESULT_REGISTER + 1));
      break;
    }
    case OPCODE_MOVE_EXCEPTION: {
      // We don't know where to grab the type of the just-caught exception.
      // Simply set to j.l.Throwable here.
      set_reference(current_state, insn->dest(), get_throwable_type());
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
      if (insn->get_literal() == 0) {
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
      set_reference(current_state, RESULT_REGISTER, get_string_type());
      break;
    }
    case OPCODE_CONST_CLASS: {
      set_reference(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_MONITOR_ENTER:
    case OPCODE_MONITOR_EXIT: {
      assume_reference(current_state, insn->src(0));
      break;
    }
    case OPCODE_CHECK_CAST: {
      assume_reference(current_state, insn->src(0));
      set_reference(current_state, RESULT_REGISTER, insn->get_type());
      break;
    }
    case OPCODE_INSTANCE_OF:
    case OPCODE_ARRAY_LENGTH: {
      assume_reference(current_state, insn->src(0));
      set_integer(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_NEW_INSTANCE: {
      set_reference(current_state, RESULT_REGISTER, insn->get_type());
      break;
    }
    case OPCODE_NEW_ARRAY: {
      assume_integer(current_state, insn->src(0));
      set_reference(current_state, RESULT_REGISTER, insn->get_type());
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
      set_reference(current_state, RESULT_REGISTER, insn->get_type());
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
      set_scalar(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_AGET_BOOLEAN:
    case OPCODE_AGET_BYTE:
    case OPCODE_AGET_CHAR:
    case OPCODE_AGET_SHORT: {
      assume_reference(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_integer(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_AGET_WIDE: {
      assume_reference(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_wide_scalar(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_AGET_OBJECT: {
      assume_reference(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      const auto dex_type_opt = current_state->get_dex_type(insn->src(0));
      if (dex_type_opt && *dex_type_opt && is_array(*dex_type_opt)) {
        const auto etype = get_array_component_type(*dex_type_opt);
        set_reference(current_state, RESULT_REGISTER, etype);
      } else {
        set_reference(current_state, RESULT_REGISTER);
      }
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
      assume_reference(current_state, insn->src(0));
      set_integer(current_state, RESULT_REGISTER);
      break;
    }
    case OPCODE_IGET_WIDE: {
      assume_reference(current_state, insn->src(0));
      const DexType* type = insn->get_field()->get_type();
      if (is_double(type)) {
        set_double(current_state, RESULT_REGISTER);
      } else {
        set_long(current_state, RESULT_REGISTER);
      }
      break;
    }
    case OPCODE_IGET_OBJECT: {
      assume_reference(current_state, insn->src(0));
      always_assert(insn->has_field());
      const auto field = insn->get_field();
      set_reference(current_state, RESULT_REGISTER, field->get_type());
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
      if (is_double(type)) {
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
      DexType* return_type = dex_method->get_proto()->get_rtype();
      if (is_void(return_type)) {
        break;
      }
      if (is_object(return_type)) {
        set_reference(current_state, RESULT_REGISTER, return_type);
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
    case OPCODE_DIV_INT:
    case OPCODE_REM_INT: {
      assume_integer(current_state, insn->src(0));
      assume_integer(current_state, insn->src(1));
      set_integer(current_state, RESULT_REGISTER);
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
      set_long(current_state, insn->dest());
      break;
    }
    case OPCODE_DIV_LONG:
    case OPCODE_REM_LONG: {
      assume_long(current_state, insn->src(0));
      assume_long(current_state, insn->src(1));
      set_long(current_state, RESULT_REGISTER);
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
      set_integer(current_state, insn->dest());
      break;
    }
    case OPCODE_DIV_INT_LIT16:
    case OPCODE_REM_INT_LIT16:
    case OPCODE_DIV_INT_LIT8:
    case OPCODE_REM_INT_LIT8: {
      assume_integer(current_state, insn->src(0));
      set_integer(current_state, RESULT_REGISTER);
      break;
    }
    }
  }

  void print(std::ostream& output) const {
    for (cfg::Block* block : m_cfg.blocks()) {
      for (auto& mie : InstructionIterable(block)) {
        IRInstruction* insn = mie.insn;
        auto it = m_type_envs.find(insn);
        always_assert(it != m_type_envs.end());
        output << SHOW(insn) << " -- " << it->second << std::endl;
      }
    }
  }

  void traceState(TypeEnvironment* state) const {
    if (!traceEnabled(TYPE, 9)) {
      return;
    }
    std::ostringstream out;
    out << *state << std::endl;
    TRACE(TYPE, 9, "%s\n", out.str().c_str());
  }

 private:
  void populate_type_environments() {
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

  void set_type(TypeEnvironment* state,
                register_t reg,
                const TypeDomain& type) const {
    if (m_inference) {
      state->set_type(reg, type);
    }
  }

  void set_integer(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(INT));
    }
  }

  void set_float(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(FLOAT));
    }
  }

  void set_scalar(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(SCALAR));
    }
  }

  void set_reference(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(REFERENCE));
    }
  }

  void set_reference(
      TypeEnvironment* state,
      register_t reg,
      const boost::optional<const DexType*>& dex_type_opt) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(REFERENCE));
      const DexTypeDomain dex_type =
          dex_type_opt ? DexTypeDomain(*dex_type_opt) : DexTypeDomain::top();
      state->set_concrete_type(reg, dex_type);
    }
  }

  void set_long(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(LONG1));
      state->set_type(reg + 1, TypeDomain(LONG2));
    }
  }

  void set_double(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(DOUBLE1));
      state->set_type(reg + 1, TypeDomain(DOUBLE2));
    }
  }

  void set_wide_scalar(TypeEnvironment* state, register_t reg) const {
    if (m_inference) {
      state->set_type(reg, TypeDomain(SCALAR1));
      state->set_type(reg + 1, TypeDomain(SCALAR2));
    }
  }

  TypeDomain refine_type(const TypeDomain& type,
                         IRType expected,
                         IRType const_type,
                         IRType scalar_type) const {
    auto refined_type = type.meet(TypeDomain(expected));
    // If constants are not considered polymorphic (the default behavior of the
    // Android verifier), we lift the constant to the type expected in the given
    // context. This only makes sense if the expected type is fully determined
    // by the context, i.e., is not a scalar type (SCALAR/SCALAR1/SCALAR2).
    if (type.leq(TypeDomain(const_type)) && expected != scalar_type) {
      return (m_enable_polymorphic_constants || refined_type.is_bottom())
                 ? refined_type
                 : TypeDomain(expected);
    }
    return refined_type;
  }

  void assume_type(TypeEnvironment* state,
                   register_t reg,
                   IRType expected,
                   bool ignore_top = false) const {
    if (m_inference) {
      state->update_type(reg, [this, expected](const TypeDomain& type) {
        return refine_type(
            type, expected, /* const_type */ CONST, /* scalar_type */ SCALAR);
      });
    } else {
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
  }

  void assume_wide_type(TypeEnvironment* state,
                        register_t reg,
                        IRType expected1,
                        IRType expected2) const {
    if (m_inference) {
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
    } else {
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
    IRType t = state->get_type(reg).element();
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
      if (m_inference) {
        // The types are incompatible and hence, the code that follows is
        // unreachable.
        state->set_to_bottom();
        return;
      }
      std::ostringstream out;
      print_register(out, reg1) << " and ";
      print_register(out, reg2)
          << ": incompatible types in comparison " << t1 << " and " << t2;
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
      print_register(out, reg)
          << ": expected type (" << expected1 << ", " << expected2
          << "), but found (" << actual1 << ", " << actual2 << ") instead";
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

  const cfg::ControlFlowGraph& m_cfg;
  bool m_enable_polymorphic_constants;
  bool m_verify_moves;
  bool m_inference;
  std::unordered_map<IRInstruction*, TypeEnvironment> m_type_envs;

  friend class ::IRTypeChecker;
};

} // namespace irtc_impl

namespace {

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
  m_type_inference = std::make_unique<irtc_impl::TypeInference>(
      cfg, m_enable_polymorphic_constants, m_verify_moves);
  m_type_inference->run(m_dex_method);

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

IRType IRTypeChecker::get_type(IRInstruction* insn, uint16_t reg) const {
  check_completion();
  auto& type_envs = m_type_inference->m_type_envs;
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
  auto& type_envs = m_type_inference->m_type_envs;
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
