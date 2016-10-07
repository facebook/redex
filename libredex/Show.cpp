/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Show.h"

#include <sstream>

#include "DexClass.h"
#include "DexAnnotation.h"
#include "DexInstruction.h"
#include "DexDebugInstruction.h"
#include "DexIdx.h"
#include "Creators.h"
#include "RegAlloc.h"
#include "Transform.h"
#include "DexUtil.h"


namespace {

std::string humanize(std::string const& type) {
  if (type.compare("B") == 0) {
    return "byte";
  } else if (type.compare("C") == 0) {
    return "char";
  } else if (type.compare("D") == 0) {
    return "double";
  } else if (type.compare("F") == 0) {
    return "float";
  } else if (type.compare("I") == 0) {
    return "int";
  } else if (type.compare("J") == 0) {
    return "long";
  } else if (type.compare("S") == 0) {
    return "short";
  } else if (type.compare("Z") == 0) {
    return "boolean";
  } else if (type[0] == '[') {
    std::stringstream ss;
    ss << humanize(type.substr(1)) << "[]";
    return ss.str();
  } else if (type[0] == 'L') {
    auto cls = type.substr(1, type.size() - 2);
    std::replace(cls.begin(), cls.end(), '/', '.');
    return cls;
  }
  return "unknonw";
}

// TODO: make sure names reported handles collisions correctly.
//       i.e. ACC_VOLATILE and ACC_BRIDGE etc.
std::string accessibility(uint32_t acc, bool method = false) {
  std::stringstream ss;
  if (acc & DexAccessFlags::ACC_PUBLIC) {
    ss << "public ";
  }
  if (acc & DexAccessFlags::ACC_PRIVATE) {
    ss << "private ";
  }
  if (acc & DexAccessFlags::ACC_PROTECTED) {
    ss << "protected ";
  }
  if (acc & DexAccessFlags::ACC_STATIC) {
    ss << "static ";
  }
  if (acc & DexAccessFlags::ACC_FINAL) {
    ss << "final ";
  }
  if (acc & DexAccessFlags::ACC_INTERFACE) {
    ss << "interface ";
  } else if (acc & DexAccessFlags::ACC_ABSTRACT) {
    ss << "abstract ";
  }
  if (acc & DexAccessFlags::ACC_ENUM) {
    ss << "enum ";
  }
  if (acc & DexAccessFlags::ACC_SYNCHRONIZED) {
    ss << "synchronized ";
  }
  if (acc & DexAccessFlags::ACC_VOLATILE) {
    if (method)
      ss << "bridge ";
    else
      ss << "volatile ";
  }
  if (acc & DexAccessFlags::ACC_NATIVE) {
    ss << "native ";
  }
  if (acc & DexAccessFlags::ACC_TRANSIENT) {
    if (method)
      ss << "vararg ";
    else
      ss << "transient ";
  }
  return ss.str();
}

std::string show(DexAnnotationVisibility vis) {
  switch (vis) {
  case DAV_BUILD:
    return "build";
  case DAV_RUNTIME:
    return "runtime";
  case DAV_SYSTEM:
    return "system";
  }
  not_reached();
}

std::string show_opcode(const DexInstruction* insn) {
  if (!insn) return "";
  std::stringstream ss;
  auto opcode = insn->opcode();
  switch (opcode) {
  case OPCODE_NOP:
    return "nop";
  case OPCODE_MOVE:
    return "move";
  case OPCODE_MOVE_WIDE:
    return "move-wide";
  case OPCODE_MOVE_OBJECT:
    return "move-object";
  case OPCODE_MOVE_RESULT:
    return "move-result";
  case OPCODE_MOVE_RESULT_WIDE:
    return "move-result-wide";
  case OPCODE_MOVE_RESULT_OBJECT:
    return "move-result-object";
  case OPCODE_MOVE_EXCEPTION:
    return "move-exception";
  case OPCODE_RETURN_VOID:
    return "return-void";
  case OPCODE_RETURN:
    return "return";
  case OPCODE_RETURN_WIDE:
    return "return-wide";
  case OPCODE_RETURN_OBJECT:
    return "return-object";
  case OPCODE_CONST_4:
    return "const/4";
  case OPCODE_MONITOR_ENTER:
    return "monitor-enter";
  case OPCODE_MONITOR_EXIT:
    return "monitor-exit";
  case OPCODE_THROW:
    return "throw";
  case OPCODE_GOTO:
    return "goto";
  case OPCODE_NEG_INT:
    return "neg-int";
  case OPCODE_NOT_INT:
    return "not-int";
  case OPCODE_NEG_LONG:
    return "neg-long";
  case OPCODE_NOT_LONG:
    return "not-long";
  case OPCODE_NEG_FLOAT:
    return "neg-float";
  case OPCODE_NEG_DOUBLE:
    return "neg-double";
  case OPCODE_INT_TO_LONG:
    return "int-to-long";
  case OPCODE_INT_TO_FLOAT:
    return "int-to-float";
  case OPCODE_INT_TO_DOUBLE:
    return "int-to-double";
  case OPCODE_LONG_TO_INT:
    return "long-to-int";
  case OPCODE_LONG_TO_FLOAT:
    return "long-to-float";
  case OPCODE_LONG_TO_DOUBLE:
    return "long-to-double";
  case OPCODE_FLOAT_TO_INT:
    return "float-to-int";
  case OPCODE_FLOAT_TO_LONG:
    return "float-to-long";
  case OPCODE_FLOAT_TO_DOUBLE:
    return "float-to-double";
  case OPCODE_DOUBLE_TO_INT:
    return "double-to-int";
  case OPCODE_DOUBLE_TO_LONG:
    return "double-to-long";
  case OPCODE_DOUBLE_TO_FLOAT:
    return "double-to-float";
  case OPCODE_INT_TO_BYTE:
    return "int-to-byte";
  case OPCODE_INT_TO_CHAR:
    return "int-to-char";
  case OPCODE_INT_TO_SHORT:
    return "int-to-short";
  case OPCODE_ADD_INT_2ADDR:
    return "add-int/2addr";
  case OPCODE_SUB_INT_2ADDR:
    return "sub-int/2addr";
  case OPCODE_MUL_INT_2ADDR:
    return "mul-int/2addr";
  case OPCODE_DIV_INT_2ADDR:
    return "div-int/2addr";
  case OPCODE_REM_INT_2ADDR:
    return "rem-int/2addr";
  case OPCODE_AND_INT_2ADDR:
    return "and-int/2addr";
  case OPCODE_OR_INT_2ADDR:
    return "or-int/2addr";
  case OPCODE_XOR_INT_2ADDR:
    return "xor-int/2addr";
  case OPCODE_SHL_INT_2ADDR:
    return "shl-int/2addr";
  case OPCODE_SHR_INT_2ADDR:
    return "shr-int/2addr";
  case OPCODE_USHR_INT_2ADDR:
    return "ushr-int/2addr";
  case OPCODE_ADD_LONG_2ADDR:
    return "add-long/2addr";
  case OPCODE_SUB_LONG_2ADDR:
    return "sub-long/2addr";
  case OPCODE_MUL_LONG_2ADDR:
    return "mul-long/2addr";
  case OPCODE_DIV_LONG_2ADDR:
    return "div-long/2addr";
  case OPCODE_REM_LONG_2ADDR:
    return "rem-long/2addr";
  case OPCODE_AND_LONG_2ADDR:
    return "and-long/2addr";
  case OPCODE_OR_LONG_2ADDR:
    return "or-long/2addr";
  case OPCODE_XOR_LONG_2ADDR:
    return "xor-long/2addr";
  case OPCODE_SHL_LONG_2ADDR:
    return "shl-long/2addr";
  case OPCODE_SHR_LONG_2ADDR:
    return "shr-long/2addr";
  case OPCODE_USHR_LONG_2ADDR:
    return "ushr-long/2addr";
  case OPCODE_ADD_FLOAT_2ADDR:
    return "add-float/2addr";
  case OPCODE_SUB_FLOAT_2ADDR:
    return "sub-float/2addr";
  case OPCODE_MUL_FLOAT_2ADDR:
    return "mul-float/2addr";
  case OPCODE_DIV_FLOAT_2ADDR:
    return "div-float/2addr";
  case OPCODE_REM_FLOAT_2ADDR:
    return "rem-float/2addr";
  case OPCODE_ADD_DOUBLE_2ADDR:
    return "add-double/2addr";
  case OPCODE_SUB_DOUBLE_2ADDR:
    return "sub-double/2addr";
  case OPCODE_MUL_DOUBLE_2ADDR:
    return "mul-double/2addr";
  case OPCODE_DIV_DOUBLE_2ADDR:
    return "div-double/2addr";
  case OPCODE_REM_DOUBLE_2ADDR:
    return "rem-double/2addr";
  case OPCODE_ARRAY_LENGTH:
    return "array-length";
  case OPCODE_MOVE_FROM16:
    return "move/from16";
  case OPCODE_MOVE_WIDE_FROM16:
    return "move-wide/from16";
  case OPCODE_MOVE_OBJECT_FROM16:
    return "move-object/from16";
  case OPCODE_CONST_16:
    return "const/16";
  case OPCODE_CONST_HIGH16:
    return "const/high16";
  case OPCODE_CONST_WIDE_16:
    return "const-wide/16";
  case OPCODE_CONST_WIDE_HIGH16:
    return "const-wide/high16";
  case OPCODE_GOTO_16:
    return "goto/16";
  case OPCODE_CMPL_FLOAT:
    return "cpml-float";
  case OPCODE_CMPG_FLOAT:
    return "cpmg-float";
  case OPCODE_CMPL_DOUBLE:
    return "cpml-double";
  case OPCODE_CMPG_DOUBLE:
    return "cpmg-double";
  case OPCODE_CMP_LONG:
    return "cpm-long";
  case OPCODE_IF_EQ:
    return "if-eq";
  case OPCODE_IF_NE:
    return "if-ne";
  case OPCODE_IF_LT:
    return "if-lt";
  case OPCODE_IF_GE:
    return "if-ge";
  case OPCODE_IF_GT:
    return "if-gt";
  case OPCODE_IF_LE:
    return "if-le";
  case OPCODE_IF_EQZ:
    return "if-eqz";
  case OPCODE_IF_NEZ:
    return "if-nez";
  case OPCODE_IF_LTZ:
    return "if-ltz";
  case OPCODE_IF_GEZ:
    return "if-gez";
  case OPCODE_IF_GTZ:
    return "if-gtz";
  case OPCODE_IF_LEZ:
    return "if-lez";
  case OPCODE_AGET:
    return "aget";
  case OPCODE_AGET_WIDE:
    return "aget-wide";
  case OPCODE_AGET_OBJECT:
    return "aget-object";
  case OPCODE_AGET_BOOLEAN:
    return "aget-boolean";
  case OPCODE_AGET_BYTE:
    return "aget-byte";
  case OPCODE_AGET_CHAR:
    return "aget-char";
  case OPCODE_AGET_SHORT:
    return "aget-short";
  case OPCODE_APUT:
    return "aput";
  case OPCODE_APUT_WIDE:
    return "aput-wide";
  case OPCODE_APUT_OBJECT:
    return "aput-object";
  case OPCODE_APUT_BOOLEAN:
    return "aput-boolean";
  case OPCODE_APUT_BYTE:
    return "aput-byte";
  case OPCODE_APUT_CHAR:
    return "aput-char";
  case OPCODE_APUT_SHORT:
    return "aput-short";
  case OPCODE_ADD_INT:
    return "add-int";
  case OPCODE_SUB_INT:
    return "sub-int";
  case OPCODE_MUL_INT:
    return "mul-int";
  case OPCODE_DIV_INT:
    return "div-int";
  case OPCODE_REM_INT:
    return "rem-int";
  case OPCODE_AND_INT:
    return "and-int";
  case OPCODE_OR_INT:
    return "or-int";
  case OPCODE_XOR_INT:
    return "xor-int";
  case OPCODE_SHL_INT:
    return "shl-int";
  case OPCODE_SHR_INT:
    return "shr-int";
  case OPCODE_USHR_INT:
    return "ushr-int";
  case OPCODE_ADD_LONG:
    return "add-long";
  case OPCODE_SUB_LONG:
    return "sub-long";
  case OPCODE_MUL_LONG:
    return "mul-long";
  case OPCODE_DIV_LONG:
    return "div-long";
  case OPCODE_REM_LONG:
    return "rem-long";
  case OPCODE_AND_LONG:
    return "and-long";
  case OPCODE_OR_LONG:
    return "or-long";
  case OPCODE_XOR_LONG:
    return "xor-long";
  case OPCODE_SHL_LONG:
    return "shl-long";
  case OPCODE_SHR_LONG:
    return "shr-long";
  case OPCODE_USHR_LONG:
    return "ushr-long";
  case OPCODE_ADD_FLOAT:
    return "add-float";
  case OPCODE_SUB_FLOAT:
    return "sub-float";
  case OPCODE_MUL_FLOAT:
    return "mul-float";
  case OPCODE_DIV_FLOAT:
    return "div-float";
  case OPCODE_REM_FLOAT:
    return "rem-float";
  case OPCODE_ADD_DOUBLE:
    return "add-double";
  case OPCODE_SUB_DOUBLE:
    return "sub-double";
  case OPCODE_MUL_DOUBLE:
    return "mul-double";
  case OPCODE_DIV_DOUBLE:
    return "div-double";
  case OPCODE_REM_DOUBLE:
    return "rem-double";
  case OPCODE_ADD_INT_LIT16:
    return "add-int/lit16";
  case OPCODE_RSUB_INT:
    return "rsub-int";
  case OPCODE_MUL_INT_LIT16:
    return "mul-int/lit16";
  case OPCODE_DIV_INT_LIT16:
    return "div-int/lit16";
  case OPCODE_REM_INT_LIT16:
    return "rem-int/lit16";
  case OPCODE_AND_INT_LIT16:
    return "and-int/lit16";
  case OPCODE_OR_INT_LIT16:
    return "or-int/lit16";
  case OPCODE_XOR_INT_LIT16:
    return "xor-int/lit16";
  case OPCODE_ADD_INT_LIT8:
    return "add-int/lit8";
  case OPCODE_RSUB_INT_LIT8:
    return "rsub-int/lit8";
  case OPCODE_MUL_INT_LIT8:
    return "mul-int/lit8";
  case OPCODE_DIV_INT_LIT8:
    return "div-int/lit8";
  case OPCODE_REM_INT_LIT8:
    return "rem-int/lit8";
  case OPCODE_AND_INT_LIT8:
    return "and-int/lit8";
  case OPCODE_OR_INT_LIT8:
    return "or-int/lit8";
  case OPCODE_XOR_INT_LIT8:
    return "xor-int/lit8";
  case OPCODE_SHL_INT_LIT8:
    return "shl-int/lit8";
  case OPCODE_SHR_INT_LIT8:
    return "shr-int/lit8";
  case OPCODE_USHR_INT_LIT8:
    return "ushr-int/lit8";
  case OPCODE_MOVE_16:
    return "move/16";
  case OPCODE_MOVE_WIDE_16:
    return "move-wide/16";
  case OPCODE_MOVE_OBJECT_16:
    return "move-object/16";
  case OPCODE_CONST:
    return "const";
  case OPCODE_CONST_WIDE_32:
    return "const-wide/32";
  case OPCODE_FILL_ARRAY_DATA:
    return "fill-array-data";
  case OPCODE_GOTO_32:
    return "goto/32";
  case OPCODE_PACKED_SWITCH:
    return "packed-switch";
  case OPCODE_SPARSE_SWITCH:
    return "sparse-switch";
  case OPCODE_CONST_WIDE:
    return "const-wide";
  // field opcode
  case OPCODE_IGET:
    ss << "iget " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IGET_WIDE:
    ss << "iget-wide " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IGET_OBJECT:
    ss << "iget-object " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IGET_BOOLEAN:
    ss << "iget-boolean " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IGET_BYTE:
    ss << "iget-byte " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IGET_CHAR:
    ss << "iget-char " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IGET_SHORT:
    ss << "iget-short " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT:
    ss << "iput " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT_WIDE:
    ss << "iput-wide " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT_OBJECT:
    ss << "iput-object " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT_BOOLEAN:
    ss << "iput-boolean " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT_BYTE:
    ss << "iput-byte " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT_CHAR:
    ss << "iput-char " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_IPUT_SHORT:
    ss << "iput-short " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET:
    ss << "sget " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET_WIDE:
    ss << "sget-wide " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET_OBJECT:
    ss << "sget-object " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET_BOOLEAN:
    ss << "sget-boolean " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET_BYTE:
    ss << "sget-byte " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET_CHAR:
    ss << "sget-char " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SGET_SHORT:
    ss << "sget-short " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT:
    ss << "sput " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT_WIDE:
    ss << "sput-wide " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT_OBJECT:
    ss << "sput-object " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT_BOOLEAN:
    ss << "sput-boolean " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT_BYTE:
    ss << "sput-byte " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT_CHAR:
    ss << "sput-char " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_SPUT_SHORT:
    ss << "sput-short " << show(((DexOpcodeField*)insn)->field());
    return ss.str();
  case OPCODE_INVOKE_VIRTUAL:
    ss << "invoke-virtual " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_SUPER:
    ss << "invoke-super " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_DIRECT:
    ss << "invoke-direct " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_STATIC:
    ss << "invoke-static " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_INTERFACE:
    ss << "invoke-interface " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_VIRTUAL_RANGE:
    ss << "invoke-virtual/range " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_SUPER_RANGE:
    ss << "invoke-super/range " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_DIRECT_RANGE:
    ss << "invoke-direct/range " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_STATIC_RANGE:
    ss << "invoke-static/range " << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_INVOKE_INTERFACE_RANGE:
    ss << "invoke-interface/range "
       << show(((DexOpcodeMethod*)insn)->get_method());
    return ss.str();
  case OPCODE_CONST_STRING:
    ss << "const-string " << show(((DexOpcodeString*)insn)->get_string());
    return ss.str();
  case OPCODE_CONST_STRING_JUMBO:
    ss << "const-string/jumbo " << show(((DexOpcodeString*)insn)->get_string());
    return ss.str();
  case OPCODE_CONST_CLASS:
    ss << "const-class " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case OPCODE_CHECK_CAST:
    ss << "check-cast " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case OPCODE_INSTANCE_OF:
    ss << "instance-of " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case OPCODE_NEW_INSTANCE:
    ss << "new-instance " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case OPCODE_NEW_ARRAY:
    ss << "new-array " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case OPCODE_FILLED_NEW_ARRAY:
    ss << "filled-new-array " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case OPCODE_FILLED_NEW_ARRAY_RANGE:
    ss << "filled-new-array/range " << show(((DexOpcodeType*)insn)->get_type());
    return ss.str();
  case FOPCODE_PACKED_SWITCH:
    ss << "packed-switch";
    return ss.str();
  case FOPCODE_SPARSE_SWITCH:
    ss << "sparse-switch";
    return ss.str();
  case FOPCODE_FILLED_ARRAY:
    ss << "filled-array";
    return ss.str();
  default:
    return "unknown_op_code";
  }
}

}

std::string show(const DexString* p) {
  if (!p) return "";
  return std::string(p->m_cstr);
}

std::string show(const DexType* p) {
  if (!p) return "";
  return show(p->m_name);
}

std::string show(const DexField* p) {
  if (!p) return "";
  std::stringstream ss;
  ss << accessibility(p->m_access) << humanize(show(p->m_ref.type)) << " "
     << humanize(show(p->m_ref.cls)) << "." << show(p->m_ref.name);
  if (p->m_anno) {
    ss << "\n  annotations:" << show(p->m_anno);
  }
  return ss.str();
}

std::string show(const DexTypeList* p) {
  if (!p) return "";
  std::stringstream ss;
  for (auto const type : p->m_list) {
    ss << show(type);
  }
  return ss.str();
}

std::string show(const DexProto* p) {
  if (!p) return "";
  std::stringstream ss;
  ss << "(" << show(p->m_args) << ")" << show(p->m_rtype);
  return ss.str();
}

std::string show(const DexCode* code) {
  if (!code) return "";
  std::stringstream ss;
  ss << "regs: " << code->m_registers_size << ", ins: " << code->m_ins_size
     << ", outs: " << code->m_outs_size << "\n";
  for (auto const& insn : code->get_instructions()) {
    ss << show(insn) << "\n";
  }
  return ss.str();
}

std::string show(const DexMethod* p) {
  if (!p) return "";
  std::stringstream ss;
  ss << accessibility(p->m_access, true) << humanize(show(p->m_ref.cls)) << "."
     << show(p->m_ref.name) << show(p->m_ref.proto);
  if (p->m_anno) {
    ss << "\n  annotations:" << show(p->m_anno);
  }
  bool first = true;
  for (auto const pair : p->m_param_anno) {
    if (first) {
      ss << "\n  param annotations:"
         << "\n";
      first = false;
    }
    ss << "    " << pair.first << ": " << show(pair.second) << "\n";
  }
  return ss.str();
}

std::string show(const DexClass* p) {
  if (!p) return "";
  std::stringstream ss;
  ss << accessibility(p->get_access()) << humanize(show(p->m_self))
     << " extends " << humanize(show(p->m_super_class));
  if (p->m_interfaces) {
    ss << " implements ";
    bool first = true;
    for (auto const type : p->m_interfaces->get_type_list()) {
      if (first)
        first = false;
      else
        ss << ", ";
      ss << humanize(show(type));
    }
  }
  if (p->m_anno) {
    ss << "\n  annotations:" << show(p->m_anno);
  }
  return ss.str();
}

std::string show(const DexEncodedValue* value) {
  if (!value) return "";
  return value->show();
}

std::string DexEncodedValue::show() const {
  std::stringstream ss;
  ss << m_value;
  return ss.str();
}

std::string DexEncodedValueArray::show() const {
  std::stringstream ss;
  ss << (m_static_val ? "(static) " : "");
  if (m_evalues) {
    bool first = true;
    for (auto const evalue : *m_evalues) {
      if (!first) ss << ' ';
      ss << evalue->show();
      first = false;
    }
  }
  return ss.str();
}

std::string DexEncodedValueAnnotation::show() const {
  std::stringstream ss;
  ss << "type:" << ::show(m_type) << " annotations:" << ::show(m_annotations);
  return ss.str();
}

std::string show(const EncodedAnnotations* annos) {
  if (!annos) return "";
  std::stringstream ss;
  bool first = true;
  for (auto const pair : *annos) {
    if (!first) ss << ", ";
    ss << show(pair.string) << ":" << pair.encoded_value->show();
    first = false;
  }
  return ss.str();
}

std::string show(const DexAnnotation* p) {
  if (!p) return "";
  std::stringstream ss;
  ss << "type:" << show(p->m_type) << " visibility:" << show(p->m_viz)
     << " annotations:" << show(&p->m_anno_elems);
  return ss.str();
}

std::string show(const DexAnnotationSet* p) {
  if (!p) return "";
  std::stringstream ss;
  bool first = true;
  for (auto const anno : p->m_annotations) {
    if (!first) ss << ", ";
    ss << show(anno);
    first = false;
  }
  return ss.str();
}

std::string show(const DexAnnotationDirectory* p) {
  if (!p) return "";
  std::stringstream ss;
  if (p->m_class) {
    ss << "class annotations:\n" << show(p->m_class) << "\n";
  }
  if (p->m_field) {
    ss << "field annotations:\n";
    for (auto const pair : *p->m_field) {
      ss << show(pair.first->get_name()) << ": " << show(pair.second) << "\n";
    }
  }
  if (p->m_method) {
    ss << "method annotations:\n";
    for (auto const pair : *p->m_method) {
      ss << show(pair.first->get_name()) << ": " << show(pair.second) << "\n";
    }
  }
  if (p->m_method_param) {
    ss << "method parameter annotations:\n";
    for (auto const pair : *p->m_method_param) {
      ss << show(pair.first->get_name());
      for (auto const parampair : *pair.second) {
        ss << "  " << parampair.first << ": " << show(parampair.second) << "\n";
      }
    }
  }
  return ss.str();
}

std::string show(DexOpcode opcode) {
  switch (opcode) {
#define OP(op, ...) \
  case OPCODE_##op: \
    return #op;
    OPS
#undef OP
        case FOPCODE_PACKED_SWITCH : return "PACKED_SWITCH_DATA";
  case FOPCODE_SPARSE_SWITCH:
    return "SPARSE_SWITCH_DATA";
  case FOPCODE_FILLED_ARRAY:
    return "FILLED_ARRAY_DATA";
  }
  always_assert_log(false, "Unknown opcode");
  return "";
}

std::string show(const DexInstruction* insn) {
  if (!insn) return "";
  std::stringstream ss;
  ss << show_opcode(insn);
  bool first = true;
  if (insn->dests_size()) {
    ss << " v" << insn->dest();
    first = false;
  }
  for (unsigned i = 0; i < insn->srcs_size(); ++i) {
    if (!first) ss << ",";
    ss << " v" << insn->src(i);
    first = false;
  }
  return ss.str();
}

std::string show(const DexDebugInstruction* insn) {
  if (!insn) return "";
  std::stringstream ss;
  switch (insn->opcode()) {
  case DBG_END_SEQUENCE:
    ss << "DBG_END_SEQUENCE";
    break;
  case DBG_ADVANCE_PC:
    ss << "DBG_ADVANCE_PC " << insn->uvalue();
    break;
  case DBG_ADVANCE_LINE:
    ss << "DBG_ADVANCE_LINE " << insn->value();
    break;
  case DBG_START_LOCAL: {
    auto sl = static_cast<const DexDebugOpcodeStartLocal*>(insn);
    ss << "DBG_START_LOCAL v" << sl->uvalue() << " " << show(sl->name())
        << ":" << show(sl->type());
    break;
  }
  case DBG_START_LOCAL_EXTENDED: {
    auto sl = static_cast<const DexDebugOpcodeStartLocal*>(insn);
    ss << "DBG_START_LOCAL v" << sl->uvalue() << " " << show(sl->name())
        << ":" << show(sl->type()) << ";" << show(sl->sig());
    break;
  }
  case DBG_END_LOCAL:
    ss << "DBG_END_LOCAL v" << insn->uvalue();
    break;
  case DBG_RESTART_LOCAL:
    ss << "DBG_RESTART_LOCAL v" << insn->uvalue();
    break;
  case DBG_SET_PROLOGUE_END:
    ss << "DBG_SET_PROLOGUE_END";
    break;
  case DBG_SET_EPILOGUE_BEGIN:
    ss << "DBG_SET_EPILOGUE_BEGIN";
    break;
  case DBG_SET_FILE: {
    auto sf = static_cast<const DexDebugOpcodeSetFile*>(insn);
    ss << "DBG_SET_FILE " << show(sf->file());
    break;
  }
  default: {
    auto adjusted_opcode = insn->opcode() - DBG_FIRST_SPECIAL;
    auto line = DBG_LINE_BASE + (adjusted_opcode % DBG_LINE_RANGE);
    auto address = (adjusted_opcode / DBG_LINE_RANGE);
    ss << "DBG_SPECIAL line+=" << line << " addr+=" << address;
  }
  }
  return ss.str();
}

std::string show(const DexPosition* pos) {
  std::stringstream ss;
  ss << show(pos->file) << ":" << pos->line;
  return ss.str();
}

std::string show(const DexDebugEntry* entry) {
  std::stringstream ss;
  ss << std::hex;
  switch (entry->type) {
    case DexDebugEntryType::Instruction:
      ss << "INSTRUCTION: [0x" << entry->addr << "] " << show(entry->insn);
      break;
    case DexDebugEntryType::Position:
      ss << "POSITION: [0x" << entry->addr << "] " << show(entry->pos);
      break;
  }
  return ss.str();
}

std::string show(TryEntryType t) {
  switch (t) {
  case TRY_START:
    return "TRY_START";
  case TRY_END:
    return "TRY_END";
  }
  not_reached();
}

std::string show(const MethodItemEntry& mei) {
  std::stringstream ss;
  switch (mei.type) {
  case MFLOW_OPCODE:
    ss << "OPCODE: [" << mei.insn << "] " << show(mei.insn);
    return ss.str();
  case MFLOW_TARGET:
    if (mei.target->type == BRANCH_MULTI) {
      ss << "TARGET MULTI: " << mei.target->src->insn;
    } else {
      ss << "TARGET SIMPLE: " << mei.target->src->insn;
    }
    return ss.str();
  case MFLOW_TRY:
    ss << "TRY: " << show(mei.tentry->type) <<
      " (CATCH: " << mei.tentry->catch_start << ")";
    return ss.str();
  case MFLOW_CATCH:
    ss << "CATCH: [" << &mei << "] " << show(mei.centry->catch_type);
    return ss.str();
  case MFLOW_DEBUG:
    ss << "DEBUG: " << show(mei.dbgop);
    return ss.str();
  case MFLOW_POSITION:
    ss << "POSITION: " << show(mei.pos);
    return ss.str();
  case MFLOW_FALLTHROUGH:
    ss << "FALLTHROUGH: [" << mei.throwing_mie << "]";
    return ss.str();
  }
  not_reached();
}

std::string show(const std::vector<Block*>& blocks) {
  std::stringstream ss;
  ss << "CFG:\n";
  for (auto& b : blocks) {
    ss << "B" << b->m_id << " succs:";
    for (auto& s : b->m_succs) {
      ss << " B" << s->m_id;
    }
    ss << " preds:";
    for (auto& p : b->m_preds) {
      ss << " B" << p->m_id;
    }
    ss << "\n";
  }
  for (auto const& b : blocks) {
    ss << "  Block B" << b->m_id << ":\n";
    for (auto const& mei : *b) {
      ss << "    " << show(mei) << "\n";
    }
  }
  return ss.str();
}

std::string show(const MethodCreator* mc) {
  if (!mc) return "";
  std::stringstream ss;
  ss << "MethodCode for " << SHOW(mc->method) << "\n";
  ss << "locals: ";
  for (auto& loc : mc->locals) {
    ss << "[" << loc.get_reg() << "] " << SHOW(loc.get_type());
  }
  ss << "\ninstructions:\n";
  ss << show(mc->main_block);
  return ss.str();
}

std::string show(const MethodBlock* block) {
  if (!block) return "";
  std::stringstream ss;
  return ss.str();
}

std::string show(DexIdx* p) {
  std::stringstream ss;
  ss << "----------------------------------------\n"
     << "strings\n"
     << "----------------------------------------\n";
  for (uint32_t i = 0; i < p->m_string_ids_size; i++) {
    ss << show(p->m_string_cache[i]) << "\n";
  }
  ss << "----------------------------------------\n"
     << "types\n"
     << "----------------------------------------\n";
  for (uint32_t i = 0; i < p->m_type_ids_size; i++) {
    ss << show(p->m_type_cache[i]) << "\n";
  }
  ss << "----------------------------------------\n"
     << "fields\n"
     << "----------------------------------------\n";
  for (uint32_t i = 0; i < p->m_field_ids_size; i++) {
    ss << show(p->m_field_cache[i]) << "\n";
  }
  ss << "----------------------------------------\n"
     << "methods\n"
     << "----------------------------------------\n";
  for (uint32_t i = 0; i < p->m_method_ids_size; i++) {
    ss << show(p->m_method_cache[i]) << "\n";
  }
  return ss.str();
}

std::string show(const Liveness& analysis) {
  std::stringstream ss;
  for (size_t i = 0; i < analysis.m_reg_set.size(); i++) {
    if (analysis.m_reg_set.test(i)) {
      ss << " " << i;
    }
  }
  return ss.str();
}
