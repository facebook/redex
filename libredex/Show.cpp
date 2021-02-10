/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"

#include <iomanip>

#include <boost/version.hpp>
// Quoted was accepted into public components as of 1.73. The `detail`
// header was removed in 1.74.
#if BOOST_VERSION < 107400
#include <boost/io/detail/quoted_manip.hpp>
#else
#include <boost/io/quoted.hpp>
#endif

#include "ControlFlow.h"
#include "CppUtil.h"
#include "Creators.h"
#include "DexAnnotation.h"
#include "DexCallSite.h"
#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "DexIdx.h"
#include "DexInstruction.h"
#include "DexMethodHandle.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "StringBuilder.h"

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
  } else if (type.compare("V") == 0) {
    return "void";
  } else if (type.compare("Z") == 0) {
    return "boolean";
  } else if (type[0] == '[') {
    std::ostringstream ss;
    ss << humanize(type.substr(1)) << "[]";
    return ss.str();
  } else if (type[0] == 'L') {
    return java_names::internal_to_external(type);
  }
  return "unknown";
}

// TODO: make sure names reported handles collisions correctly.
//       i.e. ACC_VOLATILE and ACC_BRIDGE etc.
std::string accessibility(uint32_t acc, bool method = false) {
  std::ostringstream ss;
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
}

std::string show_type(const DexType* t, bool deobfuscated) {
  return self_recursive_fn(
      [&](auto self, const DexType* t) -> std::string {
        if (t == nullptr) {
          return std::string("");
        }
        auto name = t->get_name()->str();
        if (!deobfuscated) {
          return name;
        }
        if (name[0] == 'L') {
          auto deobf_name = name;
          auto cls = type_class(t);
          if (cls != nullptr && !cls->get_deobfuscated_name().empty()) {
            deobf_name = cls->get_deobfuscated_name();
          }
          return deobf_name;
        } else if (name[0] == '[') {
          std::ostringstream ss;
          ss << '[' << self(self, DexType::get_type(name.substr(1)));
          return ss.str();
        }
        return name;
      },
      t);
}

std::string show_field(const DexFieldRef* ref, bool deobfuscated) {
  if (ref == nullptr) {
    return "";
  }

  if (deobfuscated && ref->is_def()) {
    auto name = ref->as_def()->get_deobfuscated_name();
    if (!name.empty()) {
      return name;
    }
  }
  string_builders::StaticStringBuilder<5> b;
  b << show_type(ref->get_class(), deobfuscated) << "." << show(ref->get_name())
    << ":" << show_type(ref->get_type(), deobfuscated);
  return b.str();
}

std::string show_type_list(const DexTypeList* l, bool deobfuscated) {
  if (l == nullptr) {
    return "";
  }

  const auto& type_list = l->get_type_list();
  string_builders::DynamicStringBuilder b(type_list.size());
  for (const auto& type : type_list) {
    b << show_type(type, deobfuscated);
  }
  return b.str();
}

std::string show_proto(const DexProto* p, bool deobfuscated) {
  if (p == nullptr) {
    return "";
  }
  string_builders::StaticStringBuilder<4> b;
  b << "(" << show_type_list(p->get_args(), deobfuscated) << ")"
    << show_type(p->get_rtype(), deobfuscated);
  return b.str();
}

std::string show_method(const DexMethodRef* ref, bool deobfuscated) {
  if (ref == nullptr) {
    return "";
  }

  if (deobfuscated && ref->is_def()) {
    auto name = ref->as_def()->get_deobfuscated_name();
    if (!name.empty()) {
      return name;
    }
  }

  string_builders::StaticStringBuilder<5> b;
  b << show_type(ref->get_class(), deobfuscated) << "." << show(ref->get_name())
    << ":" << show_proto(ref->get_proto(), deobfuscated);
  return b.str();
}

std::string show_opcode(const DexInstruction* insn, bool deobfuscated = false) {
  if (!insn) return "";
  std::ostringstream ss;
  auto opcode = insn->opcode();
  switch (opcode) {
  case DOPCODE_NOP:
    return "nop";
  case DOPCODE_MOVE:
    return "move";
  case DOPCODE_MOVE_WIDE:
    return "move-wide";
  case DOPCODE_MOVE_OBJECT:
    return "move-object";
  case DOPCODE_MOVE_RESULT:
    return "move-result";
  case DOPCODE_MOVE_RESULT_WIDE:
    return "move-result-wide";
  case DOPCODE_MOVE_RESULT_OBJECT:
    return "move-result-object";
  case DOPCODE_MOVE_EXCEPTION:
    return "move-exception";
  case DOPCODE_RETURN_VOID:
    return "return-void";
  case DOPCODE_RETURN:
    return "return";
  case DOPCODE_RETURN_WIDE:
    return "return-wide";
  case DOPCODE_RETURN_OBJECT:
    return "return-object";
  case DOPCODE_CONST_4:
    return "const/4";
  case DOPCODE_MONITOR_ENTER:
    return "monitor-enter";
  case DOPCODE_MONITOR_EXIT:
    return "monitor-exit";
  case DOPCODE_THROW:
    return "throw";
  case DOPCODE_GOTO:
    return "goto";
  case DOPCODE_NEG_INT:
    return "neg-int";
  case DOPCODE_NOT_INT:
    return "not-int";
  case DOPCODE_NEG_LONG:
    return "neg-long";
  case DOPCODE_NOT_LONG:
    return "not-long";
  case DOPCODE_NEG_FLOAT:
    return "neg-float";
  case DOPCODE_NEG_DOUBLE:
    return "neg-double";
  case DOPCODE_INT_TO_LONG:
    return "int-to-long";
  case DOPCODE_INT_TO_FLOAT:
    return "int-to-float";
  case DOPCODE_INT_TO_DOUBLE:
    return "int-to-double";
  case DOPCODE_LONG_TO_INT:
    return "long-to-int";
  case DOPCODE_LONG_TO_FLOAT:
    return "long-to-float";
  case DOPCODE_LONG_TO_DOUBLE:
    return "long-to-double";
  case DOPCODE_FLOAT_TO_INT:
    return "float-to-int";
  case DOPCODE_FLOAT_TO_LONG:
    return "float-to-long";
  case DOPCODE_FLOAT_TO_DOUBLE:
    return "float-to-double";
  case DOPCODE_DOUBLE_TO_INT:
    return "double-to-int";
  case DOPCODE_DOUBLE_TO_LONG:
    return "double-to-long";
  case DOPCODE_DOUBLE_TO_FLOAT:
    return "double-to-float";
  case DOPCODE_INT_TO_BYTE:
    return "int-to-byte";
  case DOPCODE_INT_TO_CHAR:
    return "int-to-char";
  case DOPCODE_INT_TO_SHORT:
    return "int-to-short";
  case DOPCODE_ARRAY_LENGTH:
    return "array-length";
  case DOPCODE_MOVE_FROM16:
    return "move/from16";
  case DOPCODE_MOVE_WIDE_FROM16:
    return "move-wide/from16";
  case DOPCODE_MOVE_OBJECT_FROM16:
    return "move-object/from16";
  case DOPCODE_CONST_16:
    return "const/16";
  case DOPCODE_CONST_HIGH16:
    return "const/high16";
  case DOPCODE_CONST_WIDE_16:
    return "const-wide/16";
  case DOPCODE_CONST_WIDE_HIGH16:
    return "const-wide/high16";
  case DOPCODE_GOTO_16:
    return "goto/16";
  case DOPCODE_CMPL_FLOAT:
    return "cmpl-float";
  case DOPCODE_CMPG_FLOAT:
    return "cmpg-float";
  case DOPCODE_CMPL_DOUBLE:
    return "cmpl-double";
  case DOPCODE_CMPG_DOUBLE:
    return "cmpg-double";
  case DOPCODE_CMP_LONG:
    return "cmp-long";
  case DOPCODE_IF_EQ:
    return "if-eq";
  case DOPCODE_IF_NE:
    return "if-ne";
  case DOPCODE_IF_LT:
    return "if-lt";
  case DOPCODE_IF_GE:
    return "if-ge";
  case DOPCODE_IF_GT:
    return "if-gt";
  case DOPCODE_IF_LE:
    return "if-le";
  case DOPCODE_IF_EQZ:
    return "if-eqz";
  case DOPCODE_IF_NEZ:
    return "if-nez";
  case DOPCODE_IF_LTZ:
    return "if-ltz";
  case DOPCODE_IF_GEZ:
    return "if-gez";
  case DOPCODE_IF_GTZ:
    return "if-gtz";
  case DOPCODE_IF_LEZ:
    return "if-lez";
  case DOPCODE_AGET:
    return "aget";
  case DOPCODE_AGET_WIDE:
    return "aget-wide";
  case DOPCODE_AGET_OBJECT:
    return "aget-object";
  case DOPCODE_AGET_BOOLEAN:
    return "aget-boolean";
  case DOPCODE_AGET_BYTE:
    return "aget-byte";
  case DOPCODE_AGET_CHAR:
    return "aget-char";
  case DOPCODE_AGET_SHORT:
    return "aget-short";
  case DOPCODE_APUT:
    return "aput";
  case DOPCODE_APUT_WIDE:
    return "aput-wide";
  case DOPCODE_APUT_OBJECT:
    return "aput-object";
  case DOPCODE_APUT_BOOLEAN:
    return "aput-boolean";
  case DOPCODE_APUT_BYTE:
    return "aput-byte";
  case DOPCODE_APUT_CHAR:
    return "aput-char";
  case DOPCODE_APUT_SHORT:
    return "aput-short";
  case DOPCODE_ADD_INT:
    return "add-int";
  case DOPCODE_SUB_INT:
    return "sub-int";
  case DOPCODE_MUL_INT:
    return "mul-int";
  case DOPCODE_DIV_INT:
    return "div-int";
  case DOPCODE_REM_INT:
    return "rem-int";
  case DOPCODE_AND_INT:
    return "and-int";
  case DOPCODE_OR_INT:
    return "or-int";
  case DOPCODE_XOR_INT:
    return "xor-int";
  case DOPCODE_SHL_INT:
    return "shl-int";
  case DOPCODE_SHR_INT:
    return "shr-int";
  case DOPCODE_USHR_INT:
    return "ushr-int";
  case DOPCODE_ADD_LONG:
    return "add-long";
  case DOPCODE_SUB_LONG:
    return "sub-long";
  case DOPCODE_MUL_LONG:
    return "mul-long";
  case DOPCODE_DIV_LONG:
    return "div-long";
  case DOPCODE_REM_LONG:
    return "rem-long";
  case DOPCODE_AND_LONG:
    return "and-long";
  case DOPCODE_OR_LONG:
    return "or-long";
  case DOPCODE_XOR_LONG:
    return "xor-long";
  case DOPCODE_SHL_LONG:
    return "shl-long";
  case DOPCODE_SHR_LONG:
    return "shr-long";
  case DOPCODE_USHR_LONG:
    return "ushr-long";
  case DOPCODE_ADD_FLOAT:
    return "add-float";
  case DOPCODE_SUB_FLOAT:
    return "sub-float";
  case DOPCODE_MUL_FLOAT:
    return "mul-float";
  case DOPCODE_DIV_FLOAT:
    return "div-float";
  case DOPCODE_REM_FLOAT:
    return "rem-float";
  case DOPCODE_ADD_DOUBLE:
    return "add-double";
  case DOPCODE_SUB_DOUBLE:
    return "sub-double";
  case DOPCODE_MUL_DOUBLE:
    return "mul-double";
  case DOPCODE_DIV_DOUBLE:
    return "div-double";
  case DOPCODE_REM_DOUBLE:
    return "rem-double";
  case DOPCODE_ADD_INT_LIT16:
    return "add-int/lit16";
  case DOPCODE_RSUB_INT:
    return "rsub-int";
  case DOPCODE_MUL_INT_LIT16:
    return "mul-int/lit16";
  case DOPCODE_DIV_INT_LIT16:
    return "div-int/lit16";
  case DOPCODE_REM_INT_LIT16:
    return "rem-int/lit16";
  case DOPCODE_AND_INT_LIT16:
    return "and-int/lit16";
  case DOPCODE_OR_INT_LIT16:
    return "or-int/lit16";
  case DOPCODE_XOR_INT_LIT16:
    return "xor-int/lit16";
  case DOPCODE_ADD_INT_LIT8:
    return "add-int/lit8";
  case DOPCODE_RSUB_INT_LIT8:
    return "rsub-int/lit8";
  case DOPCODE_MUL_INT_LIT8:
    return "mul-int/lit8";
  case DOPCODE_DIV_INT_LIT8:
    return "div-int/lit8";
  case DOPCODE_REM_INT_LIT8:
    return "rem-int/lit8";
  case DOPCODE_AND_INT_LIT8:
    return "and-int/lit8";
  case DOPCODE_OR_INT_LIT8:
    return "or-int/lit8";
  case DOPCODE_XOR_INT_LIT8:
    return "xor-int/lit8";
  case DOPCODE_SHL_INT_LIT8:
    return "shl-int/lit8";
  case DOPCODE_SHR_INT_LIT8:
    return "shr-int/lit8";
  case DOPCODE_USHR_INT_LIT8:
    return "ushr-int/lit8";
  case DOPCODE_MOVE_16:
    return "move/16";
  case DOPCODE_MOVE_WIDE_16:
    return "move-wide/16";
  case DOPCODE_MOVE_OBJECT_16:
    return "move-object/16";
  case DOPCODE_CONST:
    return "const";
  case DOPCODE_CONST_WIDE_32:
    return "const-wide/32";
  case DOPCODE_FILL_ARRAY_DATA:
    return "fill-array-data";
  case DOPCODE_GOTO_32:
    return "goto/32";
  case DOPCODE_PACKED_SWITCH:
    return "packed-switch";
  case DOPCODE_SPARSE_SWITCH:
    return "sparse-switch";
  case DOPCODE_CONST_WIDE:
    return "const-wide";
  // field opcode
  case DOPCODE_IGET:
    ss << "iget ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IGET_WIDE:
    ss << "iget-wide ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IGET_OBJECT:
    ss << "iget-object ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IGET_BOOLEAN:
    ss << "iget-boolean ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IGET_BYTE:
    ss << "iget-byte ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IGET_CHAR:
    ss << "iget-char ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IGET_SHORT:
    ss << "iget-short ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT:
    ss << "iput ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT_WIDE:
    ss << "iput-wide ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT_OBJECT:
    ss << "iput-object ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT_BOOLEAN:
    ss << "iput-boolean ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT_BYTE:
    ss << "iput-byte ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT_CHAR:
    ss << "iput-char ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_IPUT_SHORT:
    ss << "iput-short ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET:
    ss << "sget ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET_WIDE:
    ss << "sget-wide ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET_OBJECT:
    ss << "sget-object ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET_BOOLEAN:
    ss << "sget-boolean ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET_BYTE:
    ss << "sget-byte ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET_CHAR:
    ss << "sget-char ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SGET_SHORT:
    ss << "sget-short ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT:
    ss << "sput ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT_WIDE:
    ss << "sput-wide ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT_OBJECT:
    ss << "sput-object ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT_BOOLEAN:
    ss << "sput-boolean ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT_BYTE:
    ss << "sput-byte ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT_CHAR:
    ss << "sput-char ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_SPUT_SHORT:
    ss << "sput-short ";
    ss << show_field(static_cast<const DexOpcodeField*>(insn)->get_field(),
                     deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_VIRTUAL:
    ss << "invoke-virtual ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_SUPER:
    ss << "invoke-super ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_DIRECT:
    ss << "invoke-direct ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_STATIC:
    ss << "invoke-static ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_INTERFACE:
    ss << "invoke-interface ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_VIRTUAL_RANGE:
    ss << "invoke-virtual/range ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_SUPER_RANGE:
    ss << "invoke-super/range ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_DIRECT_RANGE:
    ss << "invoke-direct/range ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_STATIC_RANGE:
    ss << "invoke-static/range ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_INVOKE_INTERFACE_RANGE:
    ss << "invoke-interface/range ";
    ss << show_method(static_cast<const DexOpcodeMethod*>(insn)->get_method(),
                      deobfuscated);
    return ss.str();
  case DOPCODE_CONST_STRING:
    ss << "const-string "
       << show(static_cast<const DexOpcodeString*>(insn)->get_string());
    return ss.str();
  case DOPCODE_CONST_STRING_JUMBO:
    ss << "const-string/jumbo "
       << show(static_cast<const DexOpcodeString*>(insn)->get_string());
    return ss.str();
  case DOPCODE_CONST_CLASS:
    ss << "const-class ";
    ss << show_type(static_cast<const DexOpcodeType*>(insn)->get_type(),
                    deobfuscated);
    return ss.str();
  case DOPCODE_CHECK_CAST:
    ss << "check-cast ";
    ss << show_type(static_cast<const DexOpcodeType*>(insn)->get_type(),
                    deobfuscated);
    return ss.str();
  case DOPCODE_INSTANCE_OF:
    ss << "instance-of ";
    ss << show_type(static_cast<const DexOpcodeType*>(insn)->get_type(),
                    deobfuscated);
    return ss.str();
  case DOPCODE_NEW_INSTANCE:
    ss << "new-instance ";
    ss << show_type(static_cast<const DexOpcodeType*>(insn)->get_type(),
                    deobfuscated);
    return ss.str();
  case DOPCODE_NEW_ARRAY:
    ss << "new-array ";
    ss << show_type(static_cast<const DexOpcodeType*>(insn)->get_type(),
                    deobfuscated);
    return ss.str();
  case DOPCODE_FILLED_NEW_ARRAY:
    ss << "filled-new-array ";
    ss << show_type(static_cast<const DexOpcodeType*>(insn)->get_type(),
                    deobfuscated);
    return ss.str();
  case FOPCODE_PACKED_SWITCH:
    return "packed-switch-payload";
  case FOPCODE_SPARSE_SWITCH:
    return "sparse-switch-payload";
  case FOPCODE_FILLED_ARRAY:
    return "fill-array-data-payload";
  default:
    return "unknown_op_code";
  }
}

std::string show_insn(const IRInstruction* insn, bool deobfuscated) {
  if (!insn) return "";
  std::ostringstream ss;
  ss << show(insn->opcode()) << " ";
  bool first = true;
  if (insn->has_dest()) {
    ss << "v" << insn->dest();
    first = false;
  }
  for (unsigned i = 0; i < insn->srcs_size(); ++i) {
    if (!first) ss << ", ";
    ss << "v" << insn->src(i);
    first = false;
  }
  if (opcode::ref(insn->opcode()) != opcode::Ref::None && !first) {
    ss << ", ";
  }
  switch (opcode::ref(insn->opcode())) {
  case opcode::Ref::None:
    break;
  case opcode::Ref::String:
    ss << boost::io::quoted(show(insn->get_string()));
    break;
  case opcode::Ref::Type:
    ss << (deobfuscated ? show_deobfuscated(insn->get_type())
                        : show(insn->get_type()));
    break;
  case opcode::Ref::Field:
    if (deobfuscated) {
      ss << show_deobfuscated(insn->get_field());
    } else {
      ss << show(insn->get_field());
    }
    break;
  case opcode::Ref::Method:
    if (deobfuscated) {
      ss << show_deobfuscated(insn->get_method());
    } else {
      ss << show(insn->get_method());
    }
    break;
  case opcode::Ref::Literal:
    ss << insn->get_literal();
    break;
  case opcode::Ref::Data:
    ss << "<data>"; // TODO: print something more informative
    break;
  case opcode::Ref::CallSite:
    if (deobfuscated) {
      ss << show_deobfuscated(insn->get_callsite());
    } else {
      ss << show(insn->get_callsite());
    }
    break;
  case opcode::Ref::MethodHandle:
    if (deobfuscated) {
      ss << show_deobfuscated(insn->get_methodhandle());
    } else {
      ss << show(insn->get_methodhandle());
    }
    break;
  }
  return ss.str();
}

std::string show_helper(const DexAnnotation* anno, bool deobfuscated) {
  if (!anno) {
    return "";
  }
  std::ostringstream ss;
  ss << "type:" << show(anno->type()) << " visibility:" << show(anno->viz())
     << " annotations:";
  if (deobfuscated) {
    ss << show_deobfuscated(&anno->anno_elems());
  } else {
    ss << show(&anno->anno_elems());
  }
  return ss.str();
}

} // namespace

std::ostream& operator<<(std::ostream& o, const DexString& str) {
  o << str.c_str();
  return o;
}

// This format must match the proguard map format because it's used to look up
// in the proguard map
std::ostream& operator<<(std::ostream& o, const DexType& type) {
  o << *type.get_name();
  return o;
}

inline std::string show(DexString* p) {
  if (!p) return "";
  return p->str();
}

inline std::string show(const DexType* t) { return show_type(t, false); }

// This format must match the proguard map format because it's used to look up
// in the proguard map
std::string show(const DexFieldRef* ref) { return show_field(ref, false); }

std::ostream& operator<<(std::ostream& o, const DexFieldRef& p) {
  o << show(&p);
  return o;
}

std::string vshow(const DexField* p) {
  if (!p) return "";
  std::ostringstream ss;
  ss << accessibility(p->get_access()) << humanize(show(p->get_type())) << " "
     << humanize(show(p->get_class())) << "." << show(p->get_name());
  if (p->get_anno_set()) {
    ss << "\n  annotations:" << show(p->get_anno_set());
  }
  return ss.str();
}

std::string vshow(const DexTypeList* p) {
  if (!p) return "";
  std::ostringstream ss;
  bool first = true;
  for (auto const& type : p->get_type_list()) {
    if (!first) {
      ss << ", ";
    } else {
      first = false;
    }
    ss << humanize(show(type));
  }
  return ss.str();
}

std::string vshow(const DexProto* p, bool include_ret_type = true) {
  if (!p) return "";
  std::ostringstream ss;
  ss << "(" << vshow(p->get_args()) << ")";
  if (include_ret_type) {
    ss << humanize(show(p->get_rtype()));
  }
  return ss.str();
}

// This format must match the proguard map format because it's used to look up
// in the proguard map
std::string show(const DexTypeList* l) { return show_type_list(l, false); }

// This format must match the proguard map format because it's used to look up
// in the proguard map
std::string show(const DexProto* p) { return show_proto(p, false); }

std::string show(const DexCode* code) {
  if (!code) return "";
  std::ostringstream ss;
  ss << "regs: " << code->get_registers_size()
     << ", ins: " << code->get_ins_size() << ", outs: " << code->get_outs_size()
     << "\n";
  if (code->m_insns != nullptr) {
    for (auto const& insn : code->get_instructions()) {
      ss << show(insn) << "\n";
    }
  }
  return ss.str();
}

// This format must match the proguard map format because it's used to look up
// in the proguard map
std::string show(const DexMethodRef* ref) { return show_method(ref, false); }

std::string vshow(uint32_t acc, bool is_method) {
  return accessibility(acc, is_method);
}

std::string vshow(const DexType* t) { return humanize(show(t)); }

std::string vshow(const DexMethod* p, bool include_annotations /*=true*/) {
  if (!p) return "";
  std::ostringstream ss;
  ss << accessibility(p->get_access(), true)
     << vshow(p->get_proto()->get_rtype()) << " "
     << humanize(show(p->get_class())) << "." << show(p->get_name())
     << vshow(p->get_proto(), false);
  if (include_annotations) {
    if (p->get_anno_set()) {
      ss << "\n  annotations:" << show(p->get_anno_set());
    }
    bool first = true;
    if (p->get_param_anno() != nullptr) {
      for (auto const pair : *p->get_param_anno()) {
        if (first) {
          ss << "\n  param annotations:"
             << "\n";
          first = false;
        }
        ss << "    " << pair.first << ": " << show(pair.second) << "\n";
      }
    }
  }
  return ss.str();
}

// This format must match the proguard map format because it's used to look up
// in the proguard map
std::ostream& operator<<(std::ostream& o, const DexClass& cls) {
  o << *cls.get_type();
  return o;
}

std::string vshow(const DexClass* p) {
  if (!p) return "";
  std::ostringstream ss;
  ss << accessibility(p->get_access()) << humanize(show(p->get_type()))
     << " extends " << humanize(show(p->get_super_class()));
  if (p->get_interfaces()) {
    ss << " implements ";
    bool first = true;
    for (auto const type : p->get_interfaces()->get_type_list()) {
      if (first)
        first = false;
      else
        ss << ", ";
      ss << humanize(show(type));
    }
  }
  if (p->get_anno_set()) {
    ss << "\n  annotations:" << show(p->get_anno_set());
  }
  return ss.str();
}

std::string show(const DexEncodedValue* value) {
  if (!value) return "";
  return value->show();
}

std::string show(const DexAnnotation* anno) { return show_helper(anno, false); }

std::string show_deobfuscated(const DexAnnotation* anno) {
  return show_helper(anno, true);
}

std::string show(const DexAnnotationSet* p) {
  if (!p) return "";
  std::ostringstream ss;
  bool first = true;
  for (auto const anno : p->get_annotations()) {
    if (!first) ss << ", ";
    ss << show(anno);
    first = false;
  }
  return ss.str();
}

std::string show(const DexAnnotationDirectory* p) {
  if (!p) return "";
  std::ostringstream ss;
  if (p->m_class) {
    ss << "class annotations:\n" << show(p->m_class) << "\n";
  }
  if (p->m_field) {
    ss << "field annotations:\n";
    for (auto const& pair : *p->m_field) {
      ss << show(pair.first->get_name()) << ": " << show(pair.second) << "\n";
    }
  }
  if (p->m_method) {
    ss << "method annotations:\n";
    for (auto const& pair : *p->m_method) {
      ss << show(pair.first->get_name()) << ": " << show(pair.second) << "\n";
    }
  }
  if (p->m_method_param) {
    ss << "method parameter annotations:\n";
    for (auto const& pair : *p->m_method_param) {
      ss << show(pair.first->get_name());
      for (auto const& parampair : *pair.second) {
        ss << "  " << parampair.first << ": " << show(parampair.second) << "\n";
      }
    }
  }
  return ss.str();
}

std::string show(IROpcode opcode) {
  switch (opcode) {
#define OP(op, ...) \
  case OPCODE_##op: \
    return #op;
#define IOP(op, ...) \
  case IOPCODE_##op: \
    return "IOPCODE_" #op;
#define OPRANGE(...)
#include "IROpcodes.def"
  }
  not_reached_log("Unknown opcode 0x%x", opcode);
}

std::string show(DexOpcode opcode) {
  switch (opcode) {
#define OP(op, ...)  \
  case DOPCODE_##op: \
    return #op;
    DOPS
#undef OP
        case FOPCODE_PACKED_SWITCH : return "PACKED_SWITCH_DATA";
  case FOPCODE_SPARSE_SWITCH:
    return "SPARSE_SWITCH_DATA";
  case FOPCODE_FILLED_ARRAY:
    return "FILLED_ARRAY_DATA";
    SWITCH_FORMAT_QUICK_FIELD_REF
    SWITCH_FORMAT_QUICK_METHOD_REF
    SWITCH_FORMAT_RETURN_VOID_NO_BARRIER { not_reached(); }
  }
}

// Read n_bytes bytes from data into an integral value of type
// IntType while also incrementing the data pointer by n_bytes bytes.
// n_bytes should be less than sizeof(IntType)
template <typename IntType>
static IntType read(const uint8_t*& data, uint16_t n_bytes) {
  static_assert(std::is_integral<IntType>::value,
                "Only read into integral values.");
  always_assert_log(sizeof(IntType) >= n_bytes,
                    "Should not read more bytes than sizeof(IntType)");
  IntType result;
  memcpy(&result, data, n_bytes);
  data += n_bytes;
  return result;
}

std::string show(const DexOpcodeData* insn) {
  if (!insn) return "";
  std::ostringstream ss;
  ss << "{ ";
  const auto* data = insn->data();
  switch (insn->opcode()) {
  case FOPCODE_SPARSE_SWITCH: {
    // See format at
    // https://source.android.com/devices/tech/dalvik/dalvik-bytecode#sparse-switch
    const uint16_t entries = *data++;
    const uint16_t* tdata = data + 2 * entries;

    const uint8_t* data_ptr = (uint8_t*)data;
    const uint8_t* tdata_ptr = (uint8_t*)tdata;
    for (size_t i = 0; i < entries; i++) {
      if (i != 0) {
        ss << ", ";
      }
      int32_t case_key = read<int32_t>(data_ptr, sizeof(int32_t));
      uint32_t target_offset = read<uint32_t>(tdata_ptr, sizeof(uint32_t));
      ss << case_key << "->" << target_offset;
    }
    break;
  }
  case FOPCODE_PACKED_SWITCH: {
    // See format at
    // https://source.android.com/devices/tech/dalvik/dalvik-bytecode#packed-switch
    const uint16_t entries = *data++;
    const uint8_t* data_ptr = (uint8_t*)data;
    int32_t case_key = read<int32_t>(data_ptr, sizeof(int32_t));
    for (size_t i = 0; i < entries; i++) {
      if (i != 0) {
        ss << ", ";
      }
      uint32_t target_offset = read<uint32_t>(data_ptr, sizeof(uint32_t));
      ss << case_key++ << "->" << target_offset;
    }
    break;
  }
  case FOPCODE_FILLED_ARRAY: {
    // See format at
    // https://source.android.com/devices/tech/dalvik/dalvik-bytecode#fill-array
    const uint16_t ewidth = *data++;
    const uint32_t size = *((uint32_t*)data);
    ss << "[" << size << " x " << ewidth << "] ";
    // escape size
    data += 2;
    const uint8_t* data_ptr = (uint8_t*)data;
    ss << "{ ";
    for (size_t i = 0; i < size; i++) {
      if (i != 0) {
        ss << ", ";
      }
      ss << std::hex << read<uint64_t>(data_ptr, ewidth);
    }
    ss << " }";
    break;
  }
  default:
    // should not get here
    ss << "unknown_payload";
    break;
  }
  ss << " }";
  return ss.str();
}

std::string show_insn(const DexInstruction* insn, bool deobfuscated) {
  if (!insn) return "";
  std::ostringstream ss;
  ss << show_opcode(insn, deobfuscated);
  if (dex_opcode::is_fopcode(insn->opcode())) {
    ss << " " << show(static_cast<const DexOpcodeData*>(insn));
    return ss.str();
  }

  bool first = true;
  if (insn->has_dest()) {
    ss << " v" << insn->dest();
    first = false;
  }
  for (unsigned i = 0; i < insn->srcs_size(); ++i) {
    if (!first) ss << ",";
    ss << " v" << insn->src(i);
    first = false;
  }
  if (dex_opcode::has_literal(insn->opcode())) {
    if (!first) ss << ",";
    ss << " " << insn->get_literal();
    first = false;
  }
  return ss.str();
}

std::string show(const IRInstruction* insn) { return show_insn(insn, false); }

std::string show(const DexInstruction* insn) { return show_insn(insn, false); }

std::ostream& operator<<(std::ostream& o, const IRInstruction& insn) {
  o << show(&insn);
  return o;
}

std::string show(const DexDebugInstruction* insn) {
  if (!insn) return "";
  std::ostringstream ss;
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
    ss << "DBG_START_LOCAL v" << sl->uvalue() << " " << show(sl->name()) << ":"
       << show(sl->type());
    break;
  }
  case DBG_START_LOCAL_EXTENDED: {
    auto sl = static_cast<const DexDebugOpcodeStartLocal*>(insn);
    ss << "DBG_START_LOCAL v" << sl->uvalue() << " " << show(sl->name()) << ":"
       << show(sl->type()) << ";" << show(sl->sig());
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

std::ostream& operator<<(std::ostream& o, const DexPosition& pos) {
  if (pos.method != nullptr) {
    o << *pos.method;
  } else {
    o << "Unknown method";
  }
  o << "(";
  if (pos.file == nullptr) {
    o << "Unknown source";
  } else {
    o << *pos.file;
  }
  o << ":" << pos.line << ")";
  if (pos.parent != nullptr) {
    o << " [parent: " << pos.parent << "]";
  }
  return o;
}

std::string show(const DexDebugEntry* entry) {
  std::ostringstream ss;
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
}

std::string show(const SwitchIndices& si) {
  std::ostringstream ss;
  for (auto index : si) {
    ss << index << " ";
  }
  return ss.str();
}

std::ostream& operator<<(std::ostream& o, const MethodItemEntry& mie) {
  o << "[" << &mie << "] ";
  switch (mie.type) {
  case MFLOW_OPCODE:
    o << "OPCODE: " << show(mie.insn);
    break;
  case MFLOW_DEX_OPCODE:
    o << "DEX_OPCODE: " << show(mie.dex_insn);
    break;
  case MFLOW_TARGET:
    if (mie.target->type == BRANCH_MULTI) {
      o << "TARGET: MULTI " << mie.target->case_key << " ";
    } else {
      o << "TARGET: SIMPLE ";
    }
    o << mie.target->src;
    break;
  case MFLOW_TRY:
    o << "TRY: " << show(mie.tentry->type) << " " << mie.tentry->catch_start;
    break;
  case MFLOW_CATCH:
    o << "CATCH: " << show(mie.centry->catch_type);
    if (mie.centry->next != nullptr) {
      o << " (next " << mie.centry->next << ")";
    }
    break;
  case MFLOW_DEBUG:
    o << "DEBUG: " << show(mie.dbgop);
    break;
  case MFLOW_POSITION:
    o << "POSITION: " << *mie.pos;
    break;
  case MFLOW_SOURCE_BLOCK:
    o << "SOURCE-BLOCK: " << show(mie.src_block->src) << "@"
      << mie.src_block->id;
    break;
  case MFLOW_FALLTHROUGH:
    o << "FALLTHROUGH";
    break;
  }
  return o;
}

std::ostream& operator<<(std::ostream& o, const DexMethodHandle& mh) {
  o << "[" << &mh << "] ";
  o << "METHODHANDLE: TYPE=" << show(mh.type());
  o << " FIELD_OR_METHOD_ID=";
  if (DexMethodHandle::isInvokeType(mh.type())) {
    o << show(mh.methodref());
  } else {
    o << show(mh.fieldref());
  }
  return o;
}

std::ostream& operator<<(std::ostream& o, const DexCallSite& cs) {
  o << "[" << &cs << "] ";
  o << "CALLSITE: METHODHANDLE=" << show(cs.method_handle());
  o << " METHODNAME=" << show(cs.method_name());
  o << " METHODTYPE=" << show(cs.method_type());
  return o;
}

std::string show(const IRList* ir) {
  std::string ret;
  for (auto const& mei : *ir) {
    ret += show(mei);
    ret += "\n";
  }
  return ret;
}

std::string show(const cfg::Block* block) {
  std::ostringstream ss;
  for (const auto& mie : *block) {
    ss << "   " << show(mie) << "\n";
  }
  return ss.str();
}

std::string show(const cfg::ControlFlowGraph& cfg) {
  const auto& blocks = cfg.blocks();
  std::ostringstream ss;
  ss << "CFG:\n";
  for (const auto& b : blocks) {
    ss << " Block B" << b->id() << ":";
    if (b == cfg.entry_block()) {
      ss << " entry";
    }
    ss << "\n";

    ss << "   preds:";
    for (const auto& p : b->preds()) {
      ss << " (" << *p << " B" << p->src()->id() << ")";
    }
    ss << "\n";

    ss << show(b);

    ss << "   succs:";
    for (auto& s : b->succs()) {
      ss << " (" << *s << " B" << s->target()->id() << ")";
    }
    ss << "\n";
  }
  return ss.str();
}

std::string show(const MethodCreator* mc) {
  if (!mc) return "";
  std::ostringstream ss;
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
  std::ostringstream ss;
  return ss.str();
}

std::string show(DexIdx* p) {
  std::ostringstream ss;
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

std::string show(const IRCode* mt) { return show(mt->m_ir_list); }

std::string show(const ir_list::InstructionIterable& it) {
  std::ostringstream ss;
  for (auto& mei : it) {
    ss << show(mei.insn) << "\n";
  }
  return ss.str();
}

std::string show_context(IRCode const* code, IRInstruction const* insn) {
  std::ostringstream ss;
  auto iter = code->begin();
  while ((*iter).insn != insn) {
    always_assert(iter != code->end());
    iter++;
  }
  for (int i = 0; i < 6 && iter != code->begin(); i++) {
    iter--;
  }
  for (int i = 0; i < 11 && iter != code->end(); i++) {
    ss << SHOW(*iter++) << std::endl;
  }
  return ss.str();
}

std::string show_deobfuscated(const DexClass* cls) {
  if (!cls) {
    return "";
  }
  if (cls->get_deobfuscated_name().empty()) {
    return cls->get_name() ? cls->get_name()->str() : show(cls);
  }
  return cls->get_deobfuscated_name();
}

std::string show_deobfuscated(const DexFieldRef* ref) {
  return show_field(ref, true);
}

std::string show_deobfuscated(const DexMethodRef* ref) {
  return show_method(ref, true);
}

std::string show_deobfuscated(const IRInstruction* insn) {
  return show_insn(insn, true);
}

std::string show_deobfuscated(const DexInstruction* insn) {
  return show_insn(insn, true);
}

std::string show_deobfuscated(const DexEncodedValue* ev) {
  if (ev == nullptr) {
    return "";
  }
  return ev->show_deobfuscated();
}

std::string show_deobfuscated(const DexType* t) { return show_type(t, true); }

std::string show_deobfuscated(const DexTypeList* l) {
  return show_type_list(l, true);
}

std::string show_deobfuscated(const DexProto* p) { return show_proto(p, true); }

std::string show_deobfuscated(const DexCallSite* callsite) {
  if (!callsite) {
    return "";
  }
  // TODO(T58570881) - actually deobfuscate
  return SHOW(callsite);
}

std::string show_deobfuscated(const DexMethodHandle* methodhandle) {
  if (!methodhandle) {
    return "";
  }
  // TODO(T58570881) - actually deobfuscate
  return SHOW(methodhandle);
}

std::string pretty_bytes(uint64_t val) {
  size_t divisions = 0;
  double d_val = val;
  while (d_val > 1024 && divisions < 3) {
    d_val /= 1024;
    ++divisions;
  }

  const char* modifier;
  switch (divisions) {
  case 0:
    modifier = "";
    break;
  case 1:
    modifier = "k";
    break;
  case 2:
    modifier = "M";
    break;
  case 3:
    modifier = "G";
    break;
  default:
    modifier = "Error";
    break;
  }

  std::ostringstream oss;
  oss << std::setiosflags(std::ios::fixed) << std::setprecision(2) << d_val
      << " " << modifier << "B";
  return oss.str();
}
