/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RegisterKind.h"

#include "Dataflow.h"
#include "DexUtil.h"

std::string show(RegisterKind kind) {
  switch (kind) {
    case RegisterKind::UNKNOWN:
      return "UNKNOWN";
    case RegisterKind::NORMAL:
      return "NORMAL";
    case RegisterKind::WIDE:
      return "WIDE";
    case RegisterKind::OBJECT:
      return "OBJECT";
    case RegisterKind::MIXED:
      return "MIXED";
  }
}

RegisterKind dest_kind(DexOpcode op) {
  switch (op) {
  case OPCODE_NOP:
    always_assert_log(false, "No dest");
    not_reached();
  /* Format 10 */
  case OPCODE_MOVE:
    return RegisterKind::NORMAL;
  case OPCODE_MOVE_WIDE:
    return RegisterKind::WIDE;
  case OPCODE_MOVE_OBJECT:
    return RegisterKind::OBJECT;
  case OPCODE_MOVE_RESULT:
    return RegisterKind::NORMAL;
  case OPCODE_MOVE_RESULT_WIDE:
    return RegisterKind::WIDE;
  case OPCODE_MOVE_RESULT_OBJECT:
    return RegisterKind::OBJECT;
  case OPCODE_MOVE_EXCEPTION:
    return RegisterKind::OBJECT;
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_CONST_4:
    return RegisterKind::NORMAL;
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_THROW:
  case OPCODE_GOTO:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT:
    return RegisterKind::NORMAL;
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
    return RegisterKind::WIDE;
  case OPCODE_NEG_FLOAT:
    return RegisterKind::NORMAL;
  case OPCODE_NEG_DOUBLE:
    return RegisterKind::WIDE;
  case OPCODE_INT_TO_LONG:
    return RegisterKind::WIDE;
  case OPCODE_INT_TO_FLOAT:
    return RegisterKind::NORMAL;
  case OPCODE_INT_TO_DOUBLE:
    return RegisterKind::WIDE;
  case OPCODE_LONG_TO_INT:
  case OPCODE_LONG_TO_FLOAT:
    return RegisterKind::NORMAL;
  case OPCODE_LONG_TO_DOUBLE:
    return RegisterKind::WIDE;
  case OPCODE_FLOAT_TO_INT:
    return RegisterKind::NORMAL;
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_FLOAT_TO_DOUBLE:
    return RegisterKind::WIDE;
  case OPCODE_DOUBLE_TO_INT:
    return RegisterKind::NORMAL;
  case OPCODE_DOUBLE_TO_LONG:
    return RegisterKind::WIDE;
  case OPCODE_DOUBLE_TO_FLOAT:
    return RegisterKind::NORMAL;
  case OPCODE_INT_TO_BYTE:
  case OPCODE_INT_TO_CHAR:
  case OPCODE_INT_TO_SHORT:
    return RegisterKind::NORMAL;
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
  case OPCODE_USHR_INT_2ADDR:
  case OPCODE_ADD_LONG_2ADDR:
  case OPCODE_SUB_LONG_2ADDR:
  case OPCODE_MUL_LONG_2ADDR:
  case OPCODE_DIV_LONG_2ADDR:
  case OPCODE_REM_LONG_2ADDR:
  case OPCODE_AND_LONG_2ADDR:
  case OPCODE_OR_LONG_2ADDR:
  case OPCODE_XOR_LONG_2ADDR:
  case OPCODE_SHL_LONG_2ADDR:
  case OPCODE_SHR_LONG_2ADDR:
  case OPCODE_USHR_LONG_2ADDR:
  case OPCODE_ADD_FLOAT_2ADDR:
  case OPCODE_SUB_FLOAT_2ADDR:
  case OPCODE_MUL_FLOAT_2ADDR:
  case OPCODE_DIV_FLOAT_2ADDR:
  case OPCODE_REM_FLOAT_2ADDR:
  case OPCODE_ADD_DOUBLE_2ADDR:
  case OPCODE_SUB_DOUBLE_2ADDR:
  case OPCODE_MUL_DOUBLE_2ADDR:
  case OPCODE_DIV_DOUBLE_2ADDR:
  case OPCODE_REM_DOUBLE_2ADDR:
    always_assert_log(false, "Unhandled opcode");
    not_reached();
  case OPCODE_ARRAY_LENGTH:
    return RegisterKind::NORMAL;
  /* Format 20 */
  case OPCODE_MOVE_FROM16:
    return RegisterKind::NORMAL;
  case OPCODE_MOVE_WIDE_FROM16:
    return RegisterKind::WIDE;
  case OPCODE_MOVE_OBJECT_FROM16:
    return RegisterKind::OBJECT;
  case OPCODE_CONST_16:
  case OPCODE_CONST_HIGH16:
    return RegisterKind::NORMAL;
  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_HIGH16:
    return RegisterKind::WIDE;
  case OPCODE_GOTO_16:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT:
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_CMP_LONG:
    return RegisterKind::NORMAL;
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_AGET:
    return RegisterKind::NORMAL;
  case OPCODE_AGET_WIDE:
    return RegisterKind::WIDE;
  case OPCODE_AGET_OBJECT:
    return RegisterKind::OBJECT;
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
    return RegisterKind::NORMAL;
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
    always_assert_log(false, "No dest");
    not_reached();
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
  case OPCODE_USHR_INT:
    return RegisterKind::NORMAL;
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
    return RegisterKind::WIDE;
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT:
    return RegisterKind::NORMAL;
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
    return RegisterKind::WIDE;
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
  case OPCODE_USHR_INT_LIT8:
    return RegisterKind::NORMAL;

  /* Format 30 */
  case OPCODE_MOVE_16:
    return RegisterKind::NORMAL;
  case OPCODE_MOVE_WIDE_16:
    return RegisterKind::WIDE;
  case OPCODE_MOVE_OBJECT_16:
    return RegisterKind::OBJECT;
  case OPCODE_CONST:
    return RegisterKind::NORMAL;
  case OPCODE_CONST_WIDE_32:
    return RegisterKind::WIDE;
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_GOTO_32:
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH:
    always_assert_log(false, "No dest");
    not_reached();
  /* Format 50 */
  case OPCODE_CONST_WIDE:
    return RegisterKind::WIDE;
  /* Field ref: */
  case OPCODE_IGET:
    return RegisterKind::NORMAL;
  case OPCODE_IGET_WIDE:
    return RegisterKind::WIDE;
  case OPCODE_IGET_OBJECT:
    return RegisterKind::OBJECT;
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
    return RegisterKind::NORMAL;
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_SGET:
    return RegisterKind::NORMAL;
  case OPCODE_SGET_WIDE:
    return RegisterKind::WIDE;
  case OPCODE_SGET_OBJECT:
    return RegisterKind::OBJECT;
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
    return RegisterKind::NORMAL;
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
    always_assert_log(false, "No dest");
    not_reached();
  /* MethodRef: */
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_INVOKE_VIRTUAL_RANGE:
  case OPCODE_INVOKE_SUPER_RANGE:
  case OPCODE_INVOKE_DIRECT_RANGE:
  case OPCODE_INVOKE_STATIC_RANGE:
  case OPCODE_INVOKE_INTERFACE_RANGE:
    always_assert_log(false, "No dest");
    not_reached();
  /* StringRef: */
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_STRING_JUMBO:
  case OPCODE_CONST_CLASS:
    return RegisterKind::OBJECT;
  case OPCODE_CHECK_CAST:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_INSTANCE_OF:
    return RegisterKind::NORMAL;
  case OPCODE_NEW_INSTANCE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY_RANGE:
    return RegisterKind::OBJECT;
  case IOPCODE_LOAD_PARAM:
    return RegisterKind::NORMAL;
  case IOPCODE_LOAD_PARAM_OBJECT:
    return RegisterKind::OBJECT;
  case IOPCODE_LOAD_PARAM_WIDE:
    return RegisterKind::WIDE;
  default:
    always_assert_log(false, "Unknown opcode %02x\n", op);
  }
}

void KindVec::meet(const KindVec& that) {
  for (size_t i = 0; i < m_vec.size(); ++i) {
    if (m_vec.at(i) == RegisterKind::UNKNOWN) {
      m_vec.at(i) = that.at(i);
    } else if (that.at(i) == RegisterKind::UNKNOWN) {
      continue;
    } else if ((m_vec.at(i) == RegisterKind::NORMAL &&
                that.at(i) == RegisterKind::OBJECT) ||
               (m_vec.at(i) == RegisterKind::OBJECT &&
                that.at(i) == RegisterKind::NORMAL)) {
      // const opcodes produce values that could be used either as object
      // or non-object values... the analysis starts out assuming that they
      // are non-objects and refines that choice if the value gets used in an
      // object context
      m_vec.at(i) = RegisterKind::OBJECT;
    } else if (m_vec.at(i) != that.at(i)) {
      m_vec.at(i) = RegisterKind::MIXED;
    }
  }
}

bool KindVec::operator==(const KindVec& that) const {
  return m_vec == that.m_vec;
}

std::unique_ptr<std::unordered_map<IRInstruction*, KindVec>>
analyze_register_kinds(IRCode* code) {
  KindVec entry_kinds(code->get_registers_size());
  auto trans = [&](const IRInstruction* insn, KindVec* kinds) {
    if (insn->dests_size()) {
      (*kinds)[insn->dest()] = dest_kind(insn->opcode());
    }
  };
  code->build_cfg();
  return forwards_dataflow<KindVec>(code->cfg().blocks(),
                                    KindVec(code->get_registers_size()),
                                    trans,
                                    entry_kinds);
}
