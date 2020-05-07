/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass outlines common instruction sequences within a basic block for size
 * wins. The notion of instruction sequence equivalence is modulo register
 * names.
 *
 * At its core is a rather naive approach: check if any subsequence of
 * instructions in a block occurs sufficiently often. The average complexity is
 * held down by filtering out instruction sequences where adjacent sequences of
 * abstracted instructions ("cores") of fixed lengths never occur twice anywhere
 * in the scope (seems good enough, even without a suffix tree).
 *
 * We gather existing method/type references in a dex and make sure that we
 * don't go beyond the limits when adding methods/types, effectively filling up
 * the available ref space created by IntraDexInline (minus other reservations).
 *
 * The pass assumes that it runs after InterDex, but before RegAlloc, and
 * ideally before DedupStrings.
 *
 * There are some concessions to reduce the potential of negative runtime
 * performance impact:
 * - Performance sensitive methods (those with a weight) are not outlined
 * - Outlining happens per dex to reduce performance impact (but then later
 *   dexes in the same store can point to outlined code in an earlier dex)
 * - Outlined methods are preferably placed in the same class if all outlined
 *   sequences come from methods of a single class, or a common base class (the
 *   first one in the dex); otherwise, they are placed in a new shared helper
 *   classes (placed at the beginning of the dex)
 * - DedupStrings pass will prefer to also use the same helper class
 *
 * Safety considerations:
 * - Methods with non-minimum api levels are not outlined from.
 * - Code involving cross-store refs is not outlined.
 * - Many other technical limitations, similar in effect to inliner's technical
 *   limitations
 *
 * Ideas for future work:
 * - Retain dex positions
 * - More sophisticated normalization (commutative operations, re-ordering of
 *   independent instructions)
 * - Make outlining a bit fuzzy (e.g. pulling out constants)
 * - More aggressive cross-dex outlining
 * - Other minor TODO ideas in the code
 * - Outline beyond blocks
 */

#include "InstructionSequenceOutliner.h"

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ApiLevelChecker.h"
#include "BigBlocks.h"
#include "CFGMutation.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexLimits.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InterDexPass.h"
#include "Lazy.h"
#include "Liveness.h"
#include "MutablePriorityQueue.h"
#include "Resolver.h"
#include "Trace.h"
#include "TypeInference.h"
#include "Walkers.h"

namespace {

constexpr const char* OUTLINED_CLASS_NAME_PREFIX = "Lcom/redex/Outlined$";

// Average cost of having an outlined method reference (method_id_item,
// proto_id, type_list, string) in code units.
const size_t COST_METHOD_METADATA = 8;

// Average cost of having an outlined method body (encoded_method, code_item) in
// code units.
const size_t COST_METHOD_BODY = 8;

// Overhead of calling an outlined method with a result (invoke + move-result).
const size_t COST_INVOKE_WITH_RESULT = 4;

// Overhead of calling an outlined method without a result.
const size_t COST_INVOKE_WITHOUT_RESULT = 3;

// Maximum number of arguments in outlined methods to avoid /range instructions
const size_t MAX_ARGS = 5;

// Minimum number of instructions to be outlined in a sequence, used in
// definition of cores
const size_t MIN_INSNS_SIZE = 3;

////////////////////////////////////////////////////////////////////////////////
// "Candidate instructions" with hashes, equality, and stable hashes
////////////////////////////////////////////////////////////////////////////////

// Here, the "core" of an instruction is its opcode and associated data such as
// method/field/string/type/data/literal. This "core" concept is used for
// pruning which instruction sequences occur multiple times. Used or defined
// registers are explicitly left out as those are getting normalized.
struct CandidateInstructionCore {
  IROpcode opcode;
  union {
    DexMethodRef* method;
    DexFieldRef* field;
    DexString* string;
    DexType* type;
    DexOpcodeData* data;
    int64_t literal{0};
  };
};
static size_t hash_value(const CandidateInstructionCore& cic) {
  return cic.opcode ^ (cic.literal << 8);
}
// We define "stable hashes" for instruction sequences to create rather unique
// and stable name string for the outlined methods --- essentially, the outlined
// method name characterizes the outlined instruction sequence. We want these
// names to be stable across Redex runs, and across different Redex (and there-
// fore also boost) versions, so that name-dependent PGO remains relatively
// meaningful even with outlining enabled.
using StableHash = uint64_t;
static StableHash stable_hash_value(const std::string& s) {
  StableHash stable_hash{s.size()};
  for (auto c : s) {
    stable_hash = stable_hash * 3 + c;
  }
  return stable_hash;
}
static StableHash stable_hash_value(const CandidateInstructionCore& cic) {
  StableHash stable_hash{cic.opcode};
  switch (opcode::ref(cic.opcode)) {
  case opcode::Ref::Method:
    return stable_hash * 41 + stable_hash_value(show(cic.method));
  case opcode::Ref::Field:
    return stable_hash * 43 + stable_hash_value(show(cic.field));
  case opcode::Ref::String:
    return stable_hash * 47 + stable_hash_value(show(cic.string));
  case opcode::Ref::Type:
    return stable_hash * 53 + stable_hash_value(show(cic.type));
  case opcode::Ref::Data:
    return stable_hash * 59 + cic.data->size();
  case opcode::Ref::Literal:
    return stable_hash * 61 + cic.literal;
  default:
    return stable_hash;
  }
}
using CandidateInstructionCoreHasher = boost::hash<CandidateInstructionCore>;
bool operator==(const CandidateInstructionCore& a,
                const CandidateInstructionCore& b) {
  return a.opcode == b.opcode && a.literal == b.literal;
}

struct CandidateInstruction {
  CandidateInstructionCore core;
  std::vector<reg_t> srcs;
  boost::optional<reg_t> dest;
};
static size_t hash_value(const CandidateInstruction& ci) {
  size_t hash = hash_value(ci.core);
  boost::hash_combine(hash, boost::hash_range(ci.srcs.begin(), ci.srcs.end()));
  if (ci.dest) {
    boost::hash_combine(hash, *ci.dest);
  }
  return hash;
}
static StableHash stable_hash_value(const CandidateInstruction& ci) {
  StableHash stable_hash = stable_hash_value(ci.core);
  for (auto src : ci.srcs) {
    stable_hash = stable_hash * 3 + src;
  }
  return stable_hash;
}
using CandidateInstructionHasher = boost::hash<CandidateInstruction>;
bool operator==(const CandidateInstruction& a, const CandidateInstruction& b) {
  return a.core == b.core && a.srcs == b.srcs && a.dest == b.dest;
}

struct CandidateResult {
  const DexType* type;
  reg_t reg;
};
static size_t hash_value(const CandidateResult& cr) {
  return (size_t)cr.type ^ cr.reg;
}
using CandidateResultHasher = boost::hash<CandidateResult>;
bool operator==(const CandidateResult& a, const CandidateResult& b) {
  return a.type == b.type && a.reg == b.reg;
}

struct CandidateSequence {
  std::vector<const DexType*> arg_types;
  std::vector<CandidateInstruction> insns;
  boost::optional<CandidateResult> res;
  size_t size;
  reg_t temp_regs;
};
static size_t hash_value(const CandidateSequence& cs) {
  size_t hash = cs.size;
  if (cs.res) {
    boost::hash_combine(hash, *cs.res);
  }
  boost::hash_combine(hash,
                      boost::hash_range(cs.insns.begin(), cs.insns.end()));
  for (auto arg_type : cs.arg_types) {
    boost::hash_combine(hash, (size_t)arg_type);
  }
  return hash;
}
static StableHash stable_hash_value(const CandidateSequence& cs) {
  StableHash stable_hash{cs.arg_types.size() + cs.insns.size()};
  for (auto t : cs.arg_types) {
    stable_hash = stable_hash * 71 + stable_hash_value(show(t));
  }
  for (const auto& csi : cs.insns) {
    stable_hash = stable_hash * 73 + stable_hash_value(csi);
  }
  if (cs.res) {
    stable_hash = stable_hash * 79 + cs.res->reg;
  }
  return stable_hash;
}

using CandidateSequenceHasher = boost::hash<CandidateSequence>;
bool operator==(const CandidateSequence& a, const CandidateSequence& b) {
  if (a.arg_types != b.arg_types || a.insns != b.insns || a.res != b.res) {
    return false;
  }

  always_assert(a.size == b.size);
  always_assert(a.temp_regs == b.temp_regs);
  return true;
}

static CandidateInstructionCore to_core(IRInstruction* insn) {
  CandidateInstructionCore core{.opcode = insn->opcode()};
  if (insn->has_method()) {
    core.method = insn->get_method();
  } else if (insn->has_field()) {
    core.field = insn->get_field();
  } else if (insn->has_string()) {
    core.string = insn->get_string();
  } else if (insn->has_type()) {
    core.type = insn->get_type();
  } else if (insn->has_literal()) {
    core.literal = insn->get_literal();
  } else if (insn->has_data()) {
    core.data = insn->get_data();
  }
  return core;
}

using CandidateInstructionCores =
    std::array<CandidateInstructionCore, MIN_INSNS_SIZE>;
using CandidateInstructionCoresHasher = boost::hash<CandidateInstructionCores>;
using CandidateInstructionCoresSet =
    std::unordered_set<CandidateInstructionCores,
                       CandidateInstructionCoresHasher>;

// The cores builder efficiently keeps track of the last MIN_INSNS_SIZE many
// instructions.
class CandidateInstructionCoresBuilder {
 public:
  void push_back(IRInstruction* insn) {
    m_buffer[m_start] = to_core(insn);
    m_start = (m_start + 1) % MIN_INSNS_SIZE;
    m_size = m_size < MIN_INSNS_SIZE ? m_size + 1 : MIN_INSNS_SIZE;
  }

  void clear() { m_size = 0; }

  bool has_value() const { return m_size == MIN_INSNS_SIZE; }

  CandidateInstructionCores get_value() {
    always_assert(m_size == MIN_INSNS_SIZE);
    CandidateInstructionCores res;
    for (size_t i = 0; i < MIN_INSNS_SIZE; i++) {
      res[i] = m_buffer.at((m_start + i) % MIN_INSNS_SIZE);
    }
    return res;
  }

 private:
  std::array<CandidateInstructionCore, MIN_INSNS_SIZE> m_buffer;
  size_t m_start{0};
  size_t m_size{0};
};

////////////////////////////////////////////////////////////////////////////////
// "Partial" candidate sequences
////////////////////////////////////////////////////////////////////////////////

enum class RegState {
  // A newly created object on which no constructor was invoked yet
  UNINITIALIZED,
  // A primitive value, array, or object on which a constructor was invoked
  INITIALIZED,
  // When we don't know whether an incoming object reference has been
  // initialized (could be addressed by another analysis, but not worth it)
  UNKNOWN,
};

// A partial sequence is still evolving, and defined against an actual
// instruction sequence that hasn't been normalized yet.
struct PartialCandidateSequence {
  std::unordered_set<reg_t> in_regs;
  std::vector<IRInstruction*> insns;
  std::unordered_map<reg_t, RegState> defined_regs;
  // Approximate number of code units occupied by the instructions
  size_t size{0};
  // Number of temporary registers needed hold all the defined regs
  reg_t temp_regs{0};
};

////////////////////////////////////////////////////////////////////////////////
// Normalization of partial candidate sequence to candidate sequence
////////////////////////////////////////////////////////////////////////////////

using TypeEnvironments =
    std::unordered_map<const IRInstruction*, type_inference::TypeEnvironment>;
using LazyTypeEnvironments = Lazy<TypeEnvironments>;

// Infer type of a register at the beginning of the sequences; only as good
// as what type inference can give us.
// The return value nullptr indicates that a type could not be inferred.
static const DexType* get_initial_type(LazyTypeEnvironments& type_environments,
                                       const std::vector<IRInstruction*>& insns,
                                       reg_t reg) {
  const auto& env = type_environments->at(insns.front());
  switch (env.get_type(reg).element()) {
  case BOTTOM:
  case ZERO:
  case CONST:
  case CONST1:
  case SCALAR:
  case SCALAR1:
    // Can't figure out exact type
    return nullptr;
  case REFERENCE: {
    auto dex_type = env.get_dex_type(reg);
    return dex_type ? *dex_type : nullptr;
  }
  case INT:
    // Could actually be boolean, byte, short
    return nullptr;
  case FLOAT:
    return type::_float();
  case LONG1:
    return type::_long();
  case DOUBLE1:
    return type::_double();
  default:
    always_assert(false);
  }
}

// Infer type demand of a src register of an instruction somewhere in the
// sequence.
// The return value nullptr indicates that the demand could not be determined.
static const DexType* get_type_demand(DexMethod* method,
                                      LazyTypeEnvironments& type_environments,
                                      IRInstruction* insn,
                                      size_t src_index) {
  always_assert(src_index < insn->srcs_size());
  switch (insn->opcode()) {
  case OPCODE_GOTO:
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
  case OPCODE_NOP:
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case OPCODE_MOVE_RESULT:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case OPCODE_MOVE_RESULT_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_EXCEPTION:
  case OPCODE_RETURN_VOID:
  case OPCODE_CONST:
  case OPCODE_CONST_WIDE:
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_CLASS:
  case OPCODE_NEW_INSTANCE:
  case OPCODE_SGET:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
  case OPCODE_SGET_WIDE:
  case OPCODE_SGET_OBJECT:
    always_assert(false);

  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
    always_assert(src_index == 0);
    return method->get_proto()->get_rtype();

  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_OBJECT:
    // Handled by caller
    always_assert(false);

  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_CHECK_CAST:
  case OPCODE_INSTANCE_OF:
    always_assert(src_index == 0);
    return type::java_lang_Object();

  case OPCODE_ARRAY_LENGTH:
  case OPCODE_FILL_ARRAY_DATA: {
    always_assert(src_index == 0);
    auto& env = type_environments->at(insn);
    auto dex_type = env.get_dex_type(insn->src(0));
    return dex_type ? *dex_type : nullptr;
  }

  case OPCODE_THROW:
    always_assert(src_index == 0);
    return type::java_lang_Throwable();

  case OPCODE_IGET:
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
  case OPCODE_IGET_WIDE:
  case OPCODE_IGET_OBJECT:
    always_assert(src_index == 0);
    return insn->get_field()->get_class();

  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
    always_assert(src_index < 2);
    // Could be int, float, or object
    return nullptr;

  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
    always_assert(src_index == 0);
    // Could be int or object
    return nullptr;

  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_SWITCH:
  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT:
  case OPCODE_INT_TO_BYTE:
  case OPCODE_INT_TO_CHAR:
  case OPCODE_INT_TO_SHORT:
  case OPCODE_INT_TO_LONG:
  case OPCODE_INT_TO_FLOAT:
  case OPCODE_INT_TO_DOUBLE:
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_SHL_INT:
  case OPCODE_SHR_INT:
  case OPCODE_USHR_INT:
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT:
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_RSUB_INT:
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_ADD_INT_LIT8:
  case OPCODE_RSUB_INT_LIT8:
  case OPCODE_MUL_INT_LIT8:
  case OPCODE_SHL_INT_LIT8:
  case OPCODE_SHR_INT_LIT8:
  case OPCODE_USHR_INT_LIT8:
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8:
    always_assert(src_index < 2);
    return type::_int();

  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_AND_INT_LIT16:
  case OPCODE_OR_INT_LIT16:
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_AND_INT_LIT8:
  case OPCODE_OR_INT_LIT8:
  case OPCODE_XOR_INT_LIT8:
    always_assert(src_index < 2);
    // TODO: Note that these opcodes can preserve boolean-ness. Needs a
    // full-blown type checker.
    return nullptr;

  case OPCODE_FILLED_NEW_ARRAY:
    return type::get_array_component_type(insn->get_type());

  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT:
  case OPCODE_NEG_FLOAT:
  case OPCODE_FLOAT_TO_INT:
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_FLOAT_TO_DOUBLE:
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT:
    always_assert(src_index < 2);
    return type::_float();

  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_DOUBLE_TO_INT:
  case OPCODE_DOUBLE_TO_LONG:
  case OPCODE_DOUBLE_TO_FLOAT:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
    always_assert(src_index < 2);
    return type::_double();

  case OPCODE_CMP_LONG:
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_LONG_TO_INT:
  case OPCODE_LONG_TO_FLOAT:
  case OPCODE_LONG_TO_DOUBLE:
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
    always_assert(src_index < 2);
    return type::_long();

  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
    if (src_index == 0) return type::_long();
    always_assert(src_index == 1);
    return type::_int();

  case OPCODE_AGET:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_OBJECT:
    if (src_index == 0) {
      auto& env = type_environments->at(insn);
      auto dex_type = env.get_dex_type(insn->src(0));
      return dex_type ? *dex_type : nullptr;
    }
    always_assert(src_index == 1);
    return type::_int();

  case OPCODE_APUT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
    if (src_index == 1) {
      auto& env = type_environments->at(insn);
      auto dex_type = env.get_dex_type(insn->src(1));
      return dex_type ? *dex_type : nullptr;
    }
    if (src_index == 2) return type::_int();
    always_assert(src_index == 0);
    switch (insn->opcode()) {
    case OPCODE_APUT:
    case OPCODE_APUT_OBJECT:
    case OPCODE_APUT_WIDE: {
      auto& env = type_environments->at(insn);
      auto dex_type = env.get_dex_type(insn->src(1));
      return (dex_type && type::is_array(*dex_type))
                 ? type::get_array_component_type(*dex_type)
                 : nullptr;
    }
    case OPCODE_APUT_BOOLEAN:
      return type::_boolean();
    case OPCODE_APUT_BYTE:
      return type::_byte();
    case OPCODE_APUT_CHAR:
      return type::_char();
    case OPCODE_APUT_SHORT:
      return type::_short();
    default:
      always_assert(false);
    }

  case OPCODE_IPUT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
    if (src_index == 1) return insn->get_field()->get_class();
    always_assert(src_index == 0);
    return insn->get_field()->get_type();

  case OPCODE_SPUT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
    always_assert(src_index == 0);
    return insn->get_field()->get_type();

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

    if (insn->opcode() != OPCODE_INVOKE_STATIC) {
      // The first argument is a reference to the object instance on which the
      // method is invoked.
      if (src_index-- == 0) return dex_method->get_class();
    }
    return arg_types.at(src_index);
  }
  case OPCODE_INVOKE_CUSTOM:
  case OPCODE_INVOKE_POLYMORPHIC:
    always_assert_log(false,
                      "Unsupported instruction {%s} in "
                      "get_type_demand\n",
                      SHOW(insn));
  }
}

static bool has_dest(IRInstruction* insn, reg_t reg) {
  return insn->has_dest() && insn->dest() == reg;
}

// Infer result type of a register that will (effectively) become the result
// of an outlined sequence.
// The return value nullptr indicates that the result type could not be
// determined.
static const DexType* get_result_type(LazyTypeEnvironments& type_environments,
                                      const std::vector<IRInstruction*>& insns,
                                      size_t insn_idx) {
  auto insn = insns.at(insn_idx);
restart:
  switch (insn->opcode()) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_CLASS:
  case OPCODE_GOTO:
  case OPCODE_NOP:
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
  case OPCODE_NEW_INSTANCE:
  case OPCODE_SGET:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
  case OPCODE_SGET_WIDE:
  case OPCODE_SGET_OBJECT:
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_ARRAY_LENGTH:
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_IGET:
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
  case OPCODE_IGET_WIDE:
  case OPCODE_IGET_OBJECT:
  case OPCODE_CHECK_CAST:
  case OPCODE_INSTANCE_OF:
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_SWITCH:
  case OPCODE_FILLED_NEW_ARRAY:
  case OPCODE_AGET:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_OBJECT:
  case OPCODE_APUT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_IPUT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_SPUT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8:
    always_assert(false);

  case IOPCODE_MOVE_RESULT_PSEUDO:
  case OPCODE_MOVE_RESULT:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case OPCODE_MOVE_RESULT_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case OPCODE_MOVE_RESULT_WIDE: {
    insn = insns.at(insn_idx - 1);
    always_assert(insn != nullptr && insn->has_move_result_any());
    switch (insn->opcode()) {
    case OPCODE_CONST_STRING:
      return type::java_lang_String();
    case OPCODE_CONST_CLASS:
      return type::java_lang_Class();
    case OPCODE_NEW_INSTANCE:
    case OPCODE_NEW_ARRAY:
    case OPCODE_FILLED_NEW_ARRAY:
    case OPCODE_CHECK_CAST:
      return insn->get_type();
    case OPCODE_SGET:
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_SHORT:
    case OPCODE_SGET_WIDE:
    case OPCODE_SGET_OBJECT:
    case OPCODE_IGET:
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_SHORT:
    case OPCODE_IGET_WIDE:
    case OPCODE_IGET_OBJECT:
      return insn->get_field()->get_type();
    case OPCODE_ARRAY_LENGTH:
    case OPCODE_INSTANCE_OF:
      return type::_int();
    case OPCODE_AGET_BOOLEAN:
      return type::_boolean();
    case OPCODE_AGET_BYTE:
      return type::_byte();
    case OPCODE_AGET_CHAR:
      return type::_char();
    case OPCODE_AGET_SHORT:
      return type::_short();
    case OPCODE_AGET:
    case OPCODE_AGET_WIDE:
    case OPCODE_AGET_OBJECT: {
      auto& env = type_environments->at(insn);
      auto dex_type = env.get_dex_type(insn->src(0));
      return (dex_type && type::is_array(*dex_type))
                 ? type::get_array_component_type(*dex_type)
                 : nullptr;
    }
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_INTERFACE:
      return insn->get_method()->get_proto()->get_rtype();

    case OPCODE_DIV_INT:
    case OPCODE_REM_INT:
    case OPCODE_DIV_INT_LIT16:
    case OPCODE_REM_INT_LIT16:
    case OPCODE_DIV_INT_LIT8:
    case OPCODE_REM_INT_LIT8:
      return type::_int();
    case OPCODE_DIV_LONG:
    case OPCODE_REM_LONG:
      return type::_long();

    default:
      always_assert(false);
    }
  }

  case OPCODE_MOVE_EXCEPTION:
    return type::java_lang_Throwable();
  case OPCODE_CONST:
  case OPCODE_CONST_WIDE:
    return nullptr;

  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_OBJECT: {
    auto src = insn->src(0);
    while (insn_idx > 0) {
      insn = insns.at(--insn_idx);
      if (has_dest(insn, src)) {
        goto restart;
      }
    }
    return get_initial_type(type_environments, insns, src);
  }

  case OPCODE_THROW:
    return type::java_lang_Throwable();

  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT:
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_SHL_INT:
  case OPCODE_SHR_INT:
  case OPCODE_USHR_INT:
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_RSUB_INT:
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_ADD_INT_LIT8:
  case OPCODE_RSUB_INT_LIT8:
  case OPCODE_MUL_INT_LIT8:
  case OPCODE_SHL_INT_LIT8:
  case OPCODE_SHR_INT_LIT8:
  case OPCODE_USHR_INT_LIT8:
  case OPCODE_FLOAT_TO_INT:
  case OPCODE_DOUBLE_TO_INT:
  case OPCODE_LONG_TO_INT:
    return type::_int();

  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_AND_INT_LIT16:
  case OPCODE_OR_INT_LIT16:
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_AND_INT_LIT8:
  case OPCODE_OR_INT_LIT8:
  case OPCODE_XOR_INT_LIT8:
    // TODO: Note that these opcodes can preserve boolean-ness. Needs a
    // full-blown type checker.
    return nullptr;

  case OPCODE_INT_TO_BYTE:
    return type::_byte();
  case OPCODE_INT_TO_CHAR:
    return type::_char();
  case OPCODE_INT_TO_SHORT:
    return type::_short();
  case OPCODE_INT_TO_LONG:
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_DOUBLE_TO_LONG:
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
    return type::_long();
  case OPCODE_INT_TO_FLOAT:
  case OPCODE_NEG_FLOAT:
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT:
  case OPCODE_DOUBLE_TO_FLOAT:
  case OPCODE_LONG_TO_FLOAT:
    return type::_float();
  case OPCODE_INT_TO_DOUBLE:
  case OPCODE_FLOAT_TO_DOUBLE:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
  case OPCODE_LONG_TO_DOUBLE:
    return type::_double();

  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT:
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_CMP_LONG:
    return type::_int();

  case OPCODE_INVOKE_CUSTOM:
  case OPCODE_INVOKE_POLYMORPHIC:
    always_assert_log(false,
                      "Unsupported instruction {%s} in "
                      "get_result_type\n",
                      SHOW(insn));
  }
}

// Infer type demand imposed on an incoming register across all instructions
// in the given instruction sequence.
// The return value nullptr indicates that the demand could not be determined.
static const DexType* get_type_demand(DexMethod* method,
                                      LazyTypeEnvironments& type_environments,
                                      const std::vector<IRInstruction*>& insns,
                                      reg_t reg) {
  std::unordered_set<reg_t> regs_to_track{reg};
  std::unordered_set<const DexType*> type_demands;
  for (size_t insn_idx = 0; insn_idx < insns.size() && !regs_to_track.empty();
       insn_idx++) {
    auto insn = insns.at(insn_idx);
    if (is_move(insn->opcode())) {
      if (regs_to_track.count(insn->src(0))) {
        regs_to_track.insert(insn->dest());
      } else {
        regs_to_track.erase(insn->dest());
      }
      if (insn->opcode() == OPCODE_MOVE_WIDE) {
        regs_to_track.erase(insn->dest() + 1);
      }
      continue;
    }
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      if (regs_to_track.count(insn->src(i))) {
        type_demands.insert(
            get_type_demand(method, type_environments, insn, i));
      }
    }
    if (insn->has_dest()) {
      regs_to_track.erase(insn->dest());
      if (insn->dest_is_wide()) {
        regs_to_track.erase(insn->dest() + 1);
      }
    }
  }

  if (!type_demands.count(nullptr) && !type_demands.empty()) {
    if (type_demands.size() > 1) {
      // Less strict primitive type demands can be removed
      if (type_demands.count(type::_boolean())) {
        type_demands.erase(type::_byte());
        type_demands.erase(type::_short());
        type_demands.erase(type::_char());
        type_demands.erase(type::_int());
      } else if (type_demands.count(type::_byte())) {
        if (type_demands.count(type::_char())) {
          type_demands = {type::_int()};
        } else {
          type_demands.erase(type::_short());
          type_demands.erase(type::_int());
        }
      } else if (type_demands.count(type::_short())) {
        if (type_demands.count(type::_char())) {
          type_demands = {type::_int()};
        } else {
          type_demands.erase(type::_int());
        }
      } else if (type_demands.count(type::_char())) {
        type_demands.erase(type::_int());
      }

      // remove less specific object types
      for (auto it = type_demands.begin(); it != type_demands.end();) {
        if (type::is_object(*it) &&
            std::find_if(type_demands.begin(), type_demands.end(),
                         [&it](const DexType* t) {
                           return t != *it && type::is_object(t) &&
                                  type::check_cast(t, *it);
                         }) != type_demands.end()) {
          it = type_demands.erase(it);
        } else {
          it++;
        }
      }

      // TODO: I saw that most often, when multiple object type demands remain,
      // they are often even contradictory, and that's because in fact the
      // value that flows in is a null constant, which is the only feasible
      // value in those cases. Still, a relatively uncommon occurrence overall.
    }

    if (type_demands.size() == 1) {
      return *type_demands.begin();
    }
  }

  // No useful type demand from within the given sequence; let's fall back to
  // what we can get from type inference.
  return get_initial_type(type_environments, insns, reg);
}

// This method turns a sequence of actual instructions into a normalized
// candidate instruction sequence. The main purpose of normalization is to
// determine a canonical register assignment. Normalization also identifies the
// list and types of incoming arguments. Normalized temporary registers start
// at zero, and normalized argument registers follow after temporary registers
// in the order in which they are referenced by the instructions.
static CandidateSequence normalize(DexMethod* method,
                                   LazyTypeEnvironments& type_environments,
                                   const PartialCandidateSequence& pcs,
                                   const boost::optional<reg_t>& out_reg) {
  std::unordered_map<reg_t, reg_t> map;
  reg_t next_arg{pcs.temp_regs};
  reg_t next_temp{0};
  CandidateSequence cs{.size = pcs.size, .temp_regs = pcs.temp_regs};
  std::vector<reg_t> arg_regs;
  auto normalize_use = [&map, &next_arg, &arg_regs](reg_t reg, bool wide) {
    auto it = map.find(reg);
    if (it != map.end()) {
      return it->second;
    }
    reg_t mapped_reg = next_arg;
    next_arg += wide ? 2 : 1;
    map.emplace(reg, mapped_reg);
    arg_regs.push_back(reg);
    return mapped_reg;
  };
  auto normalize_def = [&map, &next_temp](reg_t reg, bool wide) {
    reg_t mapped_reg = next_temp;
    next_temp += wide ? 2 : 1;
    map[reg] = mapped_reg;
    return mapped_reg;
  };
  for (auto insn : pcs.insns) {
    CandidateInstruction ci{.core = to_core(insn)};
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      ci.srcs.push_back(normalize_use(insn->src(i), insn->src_is_wide(i)));
    }
    if (insn->has_dest()) {
      ci.dest = normalize_def(insn->dest(), insn->dest_is_wide());
    }
    cs.insns.push_back(ci);
  }
  always_assert(next_temp == pcs.temp_regs);
  for (auto reg : arg_regs) {
    auto type = get_type_demand(method, type_environments, pcs.insns, reg);
    cs.arg_types.push_back(type);
  }
  if (out_reg) {
    size_t out_insn_idx{pcs.insns.size() - 1};
    while (!has_dest(pcs.insns.at(out_insn_idx), *out_reg)) {
      out_insn_idx--;
    }
    auto type = get_result_type(type_environments, pcs.insns, out_insn_idx);
    cs.res = (CandidateResult){type, map.at(*out_reg)};
  }

  return cs;
}

////////////////////////////////////////////////////////////////////////////////
// find_method_candidate_sequences
////////////////////////////////////////////////////////////////////////////////

struct CandidateMethodLocation {
  IRInstruction* first_insn;
  cfg::Block* hint_block;
  // We use a linear instruction indexing scheme within a method to identify
  // ranges, which we use later to invalidate other overlapping candidates
  // while incrementally processing the most beneficial candidates using a
  // priority queue.
  size_t first_insn_idx;
};

static bool can_outline_opcode(IROpcode opcode) {
  switch (opcode) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
  case OPCODE_GOTO:
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_MOVE_EXCEPTION:
  case OPCODE_RETURN:
  case OPCODE_RETURN_OBJECT:
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN_WIDE:
  case OPCODE_SWITCH:
  case OPCODE_THROW:
    return false;

  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT:
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_CMP_LONG:
    // While these instructions could formally be part of an outlined methods,
    // we ran into issues in the past with the CSE pass, where breaking up
    // CMP and IF instructions caused some obscure issues on some Android
    // versions. So we rather avoid that. It's not a big loss.
    return false;

  default:
    return true;
  }
}

// Attempts to append an instruction to a partial candidate sequence. Result
// indicates whether attempt was successful. If not, then the partial candidate
// sequence should be abandoned.
static bool append_to_partial_candidate_sequence(
    IRInstruction* insn, PartialCandidateSequence* pcs) {
  auto opcode = insn->opcode();
  if (pcs->insns.empty() && opcode::is_move_result_any(opcode)) {
    return false;
  }
  if (opcode == OPCODE_INVOKE_DIRECT && method::is_init(insn->get_method())) {
    auto it = pcs->defined_regs.find(insn->src(0));
    if (it == pcs->defined_regs.end() || it->second == RegState::UNKNOWN) {
      return false;
    }
    it->second = RegState::INITIALIZED;
  }
  for (size_t i = 0; i < insn->srcs_size(); i++) {
    auto src = insn->src(i);
    if (!pcs->defined_regs.count(src)) {
      pcs->in_regs.insert(src);
      if (insn->src_is_wide(i)) {
        pcs->in_regs.insert(src + 1);
      }
      if (pcs->in_regs.size() > MAX_ARGS) {
        return false;
      }
    }
  }
  if (insn->has_dest()) {
    RegState reg_state = RegState::INITIALIZED;
    if (insn->opcode() == OPCODE_MOVE_OBJECT) {
      auto it = pcs->defined_regs.find(insn->src(0));
      reg_state =
          it == pcs->defined_regs.end() ? RegState::UNKNOWN : it->second;
    } else if (opcode == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT) {
      always_assert(!pcs->insns.empty());
      auto last_opcode = pcs->insns.back()->opcode();
      if (last_opcode == OPCODE_NEW_INSTANCE) {
        reg_state = RegState::UNINITIALIZED;
      }
    }
    pcs->defined_regs[insn->dest()] = reg_state;
    pcs->temp_regs += insn->dest_is_wide() ? 2 : 1;
  }
  pcs->insns.push_back(insn);
  if (!opcode::is_move(opcode)) {
    // Moves are likely still eliminated by reg-alloc or other opts
    pcs->size += insn->size();
  }
  return true;
}

using MethodCandidateSequences =
    std::unordered_map<CandidateSequence,
                       std::vector<CandidateMethodLocation>,
                       CandidateSequenceHasher>;
// For a single method, identify possible beneficial outlinable candidate
// sequences. For each sequence, gather information about where exactly in the
// given method it is located.
static MethodCandidateSequences find_method_candidate_sequences(
    const InstructionSequenceOutlinerConfig& config,
    const std::function<bool(const DexType*)>& illegal_ref,
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    const CandidateInstructionCoresSet& recurring_cores) {
  MethodCandidateSequences candidate_sequences;
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run({});
  LazyTypeEnvironments type_environments([method, &cfg]() {
    type_inference::TypeInference type_inference(cfg);
    type_inference.run(method);
    return type_inference.get_type_environments();
  });
  size_t insn_idx{0};
  // We are visiting the instructions in this method in "big block" chunks:
  // - The big blocks cover all blocks.
  // - It is safe to do so as they all share the same throw-edges, and any
  //   outlined method invocation will be placed in the first block of the big
  //   block, with the appropriate throw edges.
  for (auto& big_block : big_blocks::get_big_blocks(cfg)) {
    Lazy<std::unordered_map<IRInstruction*, LivenessDomain>> live_outs(
        [&fixpoint_iter, &big_block]() {
          std::unordered_map<IRInstruction*, LivenessDomain> res;
          for (auto block : big_block.get_blocks()) {
            auto live_out = fixpoint_iter.get_live_out_vars_at(block);
            for (auto it = block->rbegin(); it != block->rend(); ++it) {
              if (it->type != MFLOW_OPCODE) {
                continue;
              }
              res.emplace(it->insn, live_out);
              fixpoint_iter.analyze_instruction(it->insn, &live_out);
            }
          }
          return res;
        });

    // Variables that flow into throw block, if any
    Lazy<LivenessDomain> throw_live_out([&cfg, &fixpoint_iter, &big_block]() {
      auto res = LivenessDomain::bottom();
      for (auto e : cfg.get_succ_edges_of_type(big_block.get_blocks().front(),
                                               cfg::EDGE_THROW)) {
        res.join_with(fixpoint_iter.get_live_in_vars_at(e->target()));
      }
      return res;
    });

    std::list<PartialCandidateSequence> partial_candidate_sequences;
    boost::optional<IROpcode> prev_opcode;
    CandidateInstructionCoresBuilder cores_builder;
    auto ii = big_blocks::InstructionIterable(big_block);
    for (auto it = ii.begin(), end = ii.end(); it != end;
         prev_opcode = it->insn->opcode(), it++) {
      auto insn = it->insn;

      cores_builder.push_back(insn);
      if (cores_builder.has_value() &&
          !recurring_cores.count(cores_builder.get_value())) {
        // Remove all partial candidate sequences that would have the non-
        // recurring cores in them after the current instruction will have been
        // processed
        for (auto pcs_it = partial_candidate_sequences.begin();
             pcs_it != partial_candidate_sequences.end();) {
          if (pcs_it->insns.size() < MIN_INSNS_SIZE - 1) {
            ++pcs_it;
          } else {
            pcs_it = partial_candidate_sequences.erase(pcs_it);
          }
        }
      }
      insn_idx++;

      // Start a new partial candidate sequence
      partial_candidate_sequences.push_back({});

      // Append current instruction to all partial candidate sequences; prune
      // those to which cannot be appended.
      for (auto pcs_it = partial_candidate_sequences.begin();
           pcs_it != partial_candidate_sequences.end();) {
        if (pcs_it->insns.size() <= config.max_insns_size - 1 &&
            append_to_partial_candidate_sequence(insn, &*pcs_it)) {
          ++pcs_it;
        } else {
          pcs_it = partial_candidate_sequences.erase(pcs_it);
        }
      }

      // We cannot consider partial candidate sequences when they are missing
      // their move-result piece
      if (insn->has_move_result_any() &&
          !cfg.move_result_of(it.unwrap()).is_end()) {
        continue;
      }

      // We prefer not to consider sequences ending in const instructions
      if (insn->opcode() == OPCODE_CONST ||
          insn->opcode() == OPCODE_CONST_WIDE ||
          (insn->opcode() == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT && prev_opcode &&
           is_const(*prev_opcode))) {
        continue;
      }

      // At this point, we can consider all gathered partial candidate
      // sequences for nprmalization and outlining.
      // Consider normalizing a partial candidate sequence, and adding it to the
      // set of outlining candidates for this method
      for (auto& pcs : partial_candidate_sequences) {
        if (pcs.insns.size() < config.min_insns_size) {
          // Sequence is below minimum size
          continue;
        }
        if (pcs.size <= COST_INVOKE_WITHOUT_RESULT) {
          // Sequence is not longer than the replacement invoke instruction
          // would be
          continue;
        }
        boost::optional<reg_t> out_reg;
        bool unsupported_out{false};
        if (!pcs.defined_regs.empty()) {
          always_assert(insn == pcs.insns.back());
          auto& live_out = live_outs->at(insn);
          for (auto& p : pcs.defined_regs) {
            if (throw_live_out->contains(p.first)) {
              TRACE(ISO, 4,
                    "[invoke sequence outliner] [bail out] Cannot return "
                    "value that's live-out to a throw edge");
              unsupported_out = true;
              break;
            }
            if (live_out.contains(p.first)) {
              if (out_reg) {
                TRACE(ISO, 4,
                      "[invoke sequence outliner] [bail out] Cannot have more "
                      "than one out-reg");
                unsupported_out = true;
                break;
              }
              if (p.second != RegState::INITIALIZED) {
                TRACE(ISO, 4,
                      "[invoke sequence outliner] [bail out] Cannot return "
                      "uninitialized");
                unsupported_out = true;
                break;
              }
              out_reg = p.first;
            }
          }
        }
        if (unsupported_out) {
          continue;
        }
        if (out_reg && pcs.size <= COST_INVOKE_WITH_RESULT) {
          // Sequence to outlined is not longer than the replacement invoke
          // instruction would be
          continue;
        }
        auto cs = normalize(method, type_environments, pcs, out_reg);
        if (std::find(cs.arg_types.begin(), cs.arg_types.end(), nullptr) !=
            cs.arg_types.end()) {
          TRACE(ISO, 4,
                "[invoke sequence outliner] [bail out] Could not infer "
                "argument type");
          continue;
        }
        if (std::find_if(cs.arg_types.begin(), cs.arg_types.end(),
                         illegal_ref) != cs.arg_types.end()) {
          TRACE(ISO, 4,
                "[invoke sequence outliner] [bail out] Illegal argument type");
          continue;
        }
        if (cs.res && cs.res->type == nullptr) {
          TRACE(ISO, 4,
                "[invoke sequence outliner] [bail out] Could not infer "
                "result type");
          continue;
        }
        if (cs.res && illegal_ref(cs.res->type)) {
          TRACE(ISO, 4,
                "[invoke sequence outliner] [bail out] Illegal result type");
          continue;
        }
        auto& cmls = candidate_sequences[cs];
        auto first_insn_idx = insn_idx - pcs.insns.size();
        if (cmls.empty() ||
            cmls.back().first_insn_idx + pcs.insns.size() <= first_insn_idx) {
          cmls.push_back((CandidateMethodLocation){pcs.insns.front(),
                                                   it.block(), first_insn_idx});
        }
      }
    }
  }
  return candidate_sequences;
}

////////////////////////////////////////////////////////////////////////////////
// get_recurring_cores
////////////////////////////////////////////////////////////////////////////////

static bool can_outline_from_method(
    DexMethod* method,
    const std::unordered_map<std::string, unsigned int>* method_to_weight) {
  if (method->rstate.no_optimizations()) {
    return false;
  }
  if (api::LevelChecker::get_method_level(method) !=
      api::LevelChecker::get_min_level()) {
    return false;
  }
  if (method_to_weight) {
    auto cls = type_class(method->get_class());
    if (cls->is_perf_sensitive() &&
        get_method_weight_if_available(method, method_to_weight)) {
      return false;
    }
  }
  return true;
}

// Gather set of recurring small (MIN_INSNS_SIZE) adjacent instruction
// sequences that are outlinable. Note that all longer recurring outlinable
// instruction sequences must be  comprised of shorter recurring ones.
static void get_recurring_cores(
    PassManager& mgr,
    const Scope& scope,
    const std::unordered_map<std::string, unsigned int>* method_to_weight,
    const std::function<bool(const DexType*)>& illegal_ref,
    CandidateInstructionCoresSet* recurring_cores) {
  ConcurrentMap<CandidateInstructionCores, size_t,
                CandidateInstructionCoresHasher>
      concurrent_cores;
  auto legal_refs = [&illegal_ref](IRInstruction* insn) {
    std::vector<DexType*> types;
    insn->gather_types(types);
    for (const auto* t : types) {
      if (illegal_ref(t)) {
        return false;
      }
    }
    return true;
  };
  auto can_outline_insn = [legal_refs](IRInstruction* insn) {
    if (!can_outline_opcode(insn->opcode())) {
      return false;
    }
    if (insn->has_method()) {
      auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (method == nullptr) {
        return false;
      }
      if (!is_public(method) && method->is_external()) {
        return false;
      }
      if (!legal_refs(insn)) {
        return false;
      }
    } else if (insn->has_field()) {
      auto field = resolve_field(insn->get_field());
      if (field == nullptr) {
        return false;
      }
      if (!is_public(field) && field->is_external()) {
        return false;
      }
      if (!legal_refs(insn)) {
        return false;
      }
      if (is_final(field) &&
          (is_iput(insn->opcode()) || is_sput(insn->opcode()))) {
        return false;
      }
    } else if (insn->has_type()) {
      auto cls = type_class(insn->get_type());
      if (cls != nullptr) {
        if (!is_public(cls) && cls->is_external()) {
          return false;
        }
        if (!legal_refs(insn)) {
          return false;
        }
      }
    }
    return true;
  };
  walk::parallel::code(
      scope, [can_outline_insn, method_to_weight,
              &concurrent_cores](DexMethod* method, IRCode& code) {
        if (!can_outline_from_method(method, method_to_weight)) {
          return;
        }
        code.build_cfg(/* editable */ true);
        code.cfg().calculate_exit_block();
        auto& cfg = code.cfg();
        for (auto& big_block : big_blocks::get_big_blocks(cfg)) {
          CandidateInstructionCoresBuilder cores_builder;
          for (auto& mie : big_blocks::InstructionIterable(big_block)) {
            auto insn = mie.insn;
            if (!can_outline_insn(insn)) {
              cores_builder.clear();
              continue;
            }
            cores_builder.push_back(insn);
            if (cores_builder.has_value()) {
              concurrent_cores.update(cores_builder.get_value(),
                                      [](const CandidateInstructionCores&,
                                         size_t& occurrences,
                                         bool /* exists */) { occurrences++; });
            }
          }
        }
      });
  size_t singleton_cores{0};
  for (auto& p : concurrent_cores) {
    always_assert(p.second > 0);
    if (p.second > 1) {
      recurring_cores->insert(p.first);
    } else {
      singleton_cores++;
    }
  }
  mgr.incr_metric("num_singleton_cores", singleton_cores);
  mgr.incr_metric("num_recurring_cores", recurring_cores->size());
  TRACE(ISO, 2,
        "[invoke sequence outliner] %zu singleton cores, %zu recurring "
        "cores",
        singleton_cores, recurring_cores->size());
}

////////////////////////////////////////////////////////////////////////////////
// get_beneficial_candidates
////////////////////////////////////////////////////////////////////////////////

struct CandidateInfo {
  std::unordered_map<DexMethod*, std::vector<CandidateMethodLocation>> methods;
  size_t count{0};
};

// We keep track of outlined methods that reside in earlier dexes of the current
// store
using ReusableOutlinedMethods =
    std::unordered_map<CandidateSequence, DexMethod*, CandidateSequenceHasher>;

static size_t get_savings(
    const InstructionSequenceOutlinerConfig& config,
    const CandidateSequence& cs,
    const CandidateInfo& ci,
    const ReusableOutlinedMethods* reusable_outlined_methods) {
  size_t cost = cs.size * ci.count;
  size_t outlined_cost =
      COST_METHOD_METADATA +
      (cs.res ? COST_INVOKE_WITH_RESULT : COST_INVOKE_WITHOUT_RESULT) *
          ci.count;
  if (!reusable_outlined_methods || !reusable_outlined_methods->count(cs)) {
    outlined_cost += COST_METHOD_BODY + cs.size;
  }

  return (outlined_cost + config.threshold) < cost ? (cost - outlined_cost) : 0;
}

using CandidateId = uint32_t;
struct Candidate {
  CandidateSequence sequence;
  CandidateInfo info;
};

// Find beneficial candidates across all methods. Beneficial candidates are
// those that occur often enough so that there would be a net savings (in terms
// of code units / bytes) when outlining them.
// Candidates are identified by numerical candidate ids to make things
// deterministic (as opposed to a pointer) and provide an efficient
// identificiation mechanism.
static void get_beneficial_candidates(
    const InstructionSequenceOutlinerConfig& config,
    PassManager& mgr,
    const Scope& scope,
    const std::unordered_map<std::string, unsigned int>* method_to_weight,
    const std::function<bool(const DexType*)>& illegal_ref,
    const CandidateInstructionCoresSet& recurring_cores,
    const ReusableOutlinedMethods* reusable_outlined_methods,
    std::vector<Candidate>* candidates,
    std::unordered_map<DexMethod*, std::unordered_set<CandidateId>>*
        candidate_ids_by_methods) {
  ConcurrentMap<CandidateSequence, CandidateInfo, CandidateSequenceHasher>
      concurrent_candidates;

  walk::parallel::code(
      scope, [&config, method_to_weight, &illegal_ref, &recurring_cores,
              &concurrent_candidates](DexMethod* method, IRCode& code) {
        if (!can_outline_from_method(method, method_to_weight)) {
          return;
        }
        for (auto& p : find_method_candidate_sequences(
                 config, illegal_ref, method, code.cfg(), recurring_cores)) {
          std::vector<CandidateMethodLocation>& cmls = p.second;
          concurrent_candidates.update(p.first,
                                       [method, &cmls](const CandidateSequence&,
                                                       CandidateInfo& info,
                                                       bool /* exists */) {
                                         info.methods.emplace(method, cmls);
                                         info.count += cmls.size();
                                       });
        }
      });

  std::map<DexMethod*,
           std::unordered_set<CandidateSequence, CandidateSequenceHasher>,
           dexmethods_comparator>
      candidate_sequences_by_methods;
  size_t beneficial_count{0}, maleficial_count{0};
  for (auto& p : concurrent_candidates) {
    if (get_savings(config, p.first, p.second, reusable_outlined_methods) > 0) {
      beneficial_count += p.second.count;
      for (auto& q : p.second.methods) {
        candidate_sequences_by_methods[q.first].insert(p.first);
      }
    } else {
      maleficial_count += p.second.count;
    }
  }
  TRACE(ISO, 2,
        "[invoke sequence outliner] %zu beneficial candidates, %zu "
        "maleficial candidates",
        beneficial_count, maleficial_count);
  mgr.incr_metric("num_beneficial_candidates", beneficial_count);
  mgr.incr_metric("num_maleficial_candidates", maleficial_count);
  // Deterministically compute unique candidate ids
  std::unordered_map<CandidateSequence, CandidateId, CandidateSequenceHasher>
      candidate_ids;
  for (auto& p : candidate_sequences_by_methods) {
    auto& method_candidate_ids = (*candidate_ids_by_methods)[p.first];
    std::map<size_t, std::map<size_t, CandidateSequence>> ordered;
    for (auto& cs : p.second) {
      auto it = candidate_ids.find(cs);
      if (it != candidate_ids.end()) {
        method_candidate_ids.insert(it->second);
        continue;
      }
      const auto& ci = concurrent_candidates.at_unsafe(cs);
      for (auto& cl : ci.methods.at(p.first)) {
        ordered[cl.first_insn_idx].emplace(cs.insns.size(), cs);
      }
    }
    for (auto& q : ordered) {
      for (auto& r : q.second) {
        if (!candidate_ids.count(r.second)) {
          always_assert(candidate_ids.size() < (1ULL << 32));
          CandidateId candidate_id = candidate_ids.size();
          method_candidate_ids.insert(candidate_id);
          candidate_ids.emplace(r.second, candidate_id);
          candidates->push_back(
              {r.second, concurrent_candidates.at_unsafe(r.second)});
        }
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// outline
////////////////////////////////////////////////////////////////////////////////

static bool has_non_init_invoke_directs(const CandidateSequence& cs) {
  for (const auto& csi : cs.insns) {
    if (csi.core.opcode == OPCODE_INVOKE_DIRECT &&
        !method::is_init(csi.core.method)) {
      return true;
    }
  }
  return false;
}

// A name generator for outlined methods
class MethodNameGenerator {
 private:
  PassManager& m_mgr;
  std::unordered_map<DexType*, std::unordered_map<StableHash, size_t>>
      m_unique_method_ids;
  size_t m_max_unique_method_id{0};

 public:
  MethodNameGenerator() = delete;
  MethodNameGenerator(const MethodNameGenerator&) = delete;
  MethodNameGenerator& operator=(const MethodNameGenerator&) = delete;
  explicit MethodNameGenerator(PassManager& mgr) : m_mgr(mgr) {}

  // Compute the name of the outlined method in a way that tends to be stable
  // across Redex runs.
  DexString* get_name(DexType* host_class, const CandidateSequence& cs) {
    StableHash stable_hash = stable_hash_value(cs);
    auto unique_method_id = m_unique_method_ids[host_class][stable_hash]++;
    m_max_unique_method_id = std::max(m_max_unique_method_id, unique_method_id);
    std::string name("$outlined$");
    name += std::to_string(stable_hash);
    if (unique_method_id > 0) {
      name += std::string("$") + std::to_string(unique_method_id);
      TRACE(ISO, 5,
            "[invoke sequence outliner] name with non-unique stable id: %s",
            name.c_str());
    }
    return DexString::make_string(name);
  }

  ~MethodNameGenerator() {
    m_mgr.incr_metric("max_unique_method_id", m_max_unique_method_id);
    TRACE(ISO, 2, "[invoke sequence outliner] %zu max unique method id",
          m_max_unique_method_id);
  }
};

class OutlinedMethodCreator {
 private:
  PassManager& m_mgr;
  MethodNameGenerator& m_method_name_generator;
  size_t m_outlined_methods{0};
  size_t m_outlined_method_instructions{0};

  // Construct an IRCode datastructure from a candidate sequence.
  std::unique_ptr<IRCode> get_outlined_code(DexMethod* outlined_method,
                                            const CandidateSequence& cs) {
    auto code = std::make_unique<IRCode>(outlined_method, cs.temp_regs);
    for (auto& ci : cs.insns) {
      auto insn = new IRInstruction(ci.core.opcode);
      insn->set_srcs_size(ci.srcs.size());
      for (size_t i = 0; i < ci.srcs.size(); i++) {
        insn->set_src(i, ci.srcs.at(i));
      }
      if (ci.dest) {
        insn->set_dest(*ci.dest);
      }
      if (insn->has_method()) {
        insn->set_method(ci.core.method);
      } else if (insn->has_field()) {
        insn->set_field(ci.core.field);
      } else if (insn->has_string()) {
        insn->set_string(ci.core.string);
      } else if (insn->has_type()) {
        insn->set_type(ci.core.type);
      } else if (insn->has_literal()) {
        insn->set_literal(ci.core.literal);
      } else if (insn->has_data()) {
        insn->set_data(ci.core.data);
      }
      code->push_back(insn);
    }
    m_outlined_method_instructions += cs.insns.size();
    if (cs.res) {
      IROpcode ret_opcode = type::is_object(cs.res->type)
                                ? OPCODE_RETURN_OBJECT
                                : type::is_wide_type(cs.res->type)
                                      ? OPCODE_RETURN_WIDE
                                      : OPCODE_RETURN;
      auto ret_insn = new IRInstruction(ret_opcode);
      ret_insn->set_src(0, cs.res->reg);
      code->push_back(ret_insn);
    } else {
      auto ret_insn = new IRInstruction(OPCODE_RETURN_VOID);
      code->push_back(ret_insn);
    }
    return code;
  }

 public:
  OutlinedMethodCreator() = delete;
  OutlinedMethodCreator(const OutlinedMethodCreator&) = delete;
  OutlinedMethodCreator& operator=(const OutlinedMethodCreator&) = delete;
  explicit OutlinedMethodCreator(PassManager& mgr,
                                 MethodNameGenerator& method_name_generator)
      : m_mgr(mgr), m_method_name_generator(method_name_generator) {}

  // Obtain outlined method for a sequence.
  DexMethod* create_outlined_method(const CandidateSequence& cs,
                                    DexType* host_class) {
    auto name = m_method_name_generator.get_name(host_class, cs);
    std::deque<DexType*> arg_types;
    for (auto t : cs.arg_types) {
      arg_types.push_back(const_cast<DexType*>(t));
    }
    auto rtype = cs.res ? cs.res->type : type::_void();
    auto type_list = DexTypeList::make_type_list(std::move(arg_types));
    auto proto = DexProto::make_proto(rtype, type_list);
    auto outlined_method = DexMethod::make_method(host_class, name, proto)
                               ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    outlined_method->set_code(get_outlined_code(outlined_method, cs));
    outlined_method->set_deobfuscated_name(show(outlined_method));
    outlined_method->rstate.set_dont_inline();
    change_visibility(outlined_method->get_code(), host_class);
    type_class(host_class)->add_method(outlined_method);
    TRACE(ISO, 5, "[invoke sequence outliner] outlined to %s\n%s",
          SHOW(outlined_method), SHOW(outlined_method->get_code()));
    m_outlined_methods++;
    return outlined_method;
  }

  ~OutlinedMethodCreator() {
    m_mgr.incr_metric("num_outlined_methods", m_outlined_methods);
    m_mgr.incr_metric("num_outlined_method_instructions",
                      m_outlined_method_instructions);
    TRACE(
        ISO, 2,
        "[invoke sequence outliner] %zu outlined methods with %zu instructions",
        m_outlined_methods, m_outlined_method_instructions);
  }
};

// Rewrite instruction sequence in existing method to invoke an outlined
// method instead.
static void rewrite_sequence_at_location(DexMethod* outlined_method,
                                         cfg::ControlFlowGraph& cfg,
                                         const CandidateSequence& cs,
                                         const CandidateMethodLocation& cml) {
  // Figure out argument and result registers
  auto first_insn_it = cfg.find_insn(cml.first_insn, cml.hint_block);
  if (first_insn_it.is_end()) {
    // This should not happen, as for each candidate we never produce
    // overlapping locations in a method, and overlaps across selected
    // candidates are prevented by meticulously removing remaining overlapping
    // occurrences after processing a candidate.
    always_assert(false);
  }
  cfg::CFGMutation cfg_mutation(cfg);
  std::vector<reg_t> arg_regs;
  boost::optional<reg_t> res_reg;
  boost::optional<reg_t> highest_mapped_arg_reg;
  auto it = big_blocks::InstructionIterator(first_insn_it);
  for (size_t insn_idx = 0; insn_idx < cs.insns.size(); insn_idx++, it++) {
    auto& ci = cs.insns.at(insn_idx);
    always_assert(it->insn->opcode() == ci.core.opcode);
    for (size_t i = 0; i < ci.srcs.size(); i++) {
      auto mapped_reg = ci.srcs.at(i);
      if (mapped_reg >= cs.temp_regs &&
          (!highest_mapped_arg_reg || mapped_reg > *highest_mapped_arg_reg)) {
        highest_mapped_arg_reg = mapped_reg;
        arg_regs.push_back(it->insn->src(i));
      }
    }
    if (ci.dest && cs.res && cs.res->reg == *ci.dest) {
      res_reg = it->insn->dest();
    }
    if (!opcode::is_move_result_any(it->insn->opcode())) {
      cfg_mutation.remove(it.unwrap());
    }
  }
  // Generate and insert invocation instructions
  std::vector<IRInstruction*> outlined_method_invocation;
  IRInstruction* invoke_insn = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_insn->set_method(outlined_method);
  invoke_insn->set_srcs_size(arg_regs.size());
  for (size_t i = 0; i < arg_regs.size(); i++) {
    invoke_insn->set_src(i, arg_regs.at(i));
  }
  outlined_method_invocation.push_back(invoke_insn);
  if (cs.res) {
    IRInstruction* move_result_insn =
        new IRInstruction(opcode::move_result_for_invoke(outlined_method));
    move_result_insn->set_dest(*res_reg);
    outlined_method_invocation.push_back(move_result_insn);
  }
  cfg_mutation.insert_before(first_insn_it, outlined_method_invocation);
  cfg_mutation.flush();
};

// Manages references and assigns numeric ids to classes
// We don't want to use more methods or types than are available, so we gather
// all already used references in the given scope.
class DexState {
 private:
  PassManager& m_mgr;
  DexClasses& m_dex;
  size_t m_dex_id;
  std::unordered_set<DexType*> m_type_refs;
  size_t m_method_refs_count;
  std::unordered_map<DexType*, size_t> m_class_ids;

 public:
  DexState() = delete;
  DexState(const DexState&) = delete;
  DexState& operator=(const DexState&) = delete;
  DexState(PassManager& mgr,
           DexClasses& dex,
           size_t dex_id,
           size_t reserved_mrefs)
      : m_mgr(mgr), m_dex(dex), m_dex_id(dex_id) {
    std::vector<DexMethodRef*> method_refs;
    std::vector<DexType*> type_refs;
    for (auto cls : dex) {
      cls->gather_methods(method_refs);
      cls->gather_types(type_refs);
    }
    sort_unique(method_refs);
    m_method_refs_count = method_refs.size() + reserved_mrefs;
    m_type_refs.insert(type_refs.begin(), type_refs.end());

    walk::classes(dex, [& class_ids = m_class_ids](DexClass* cls) {
      class_ids.emplace(cls->get_type(), class_ids.size());
    });
  }

  size_t get_dex_id() { return m_dex_id; }

  bool can_insert_type_refs(const std::unordered_set<DexType*>& types) {
    size_t inserted_count{0};
    for (auto t : types) {
      if (!m_type_refs.count(t)) {
        inserted_count++;
      }
    }
    // Yes, looks a bit quirky, but matching what happens elsewhere: The number
    // of type refs must stay *below* the maximum, and must never reach it.
    if ((m_type_refs.size() + inserted_count) >= kMaxTypeRefs) {
      m_mgr.incr_metric("kMaxTypeRefs", 1);
      TRACE(ISO, 2, "[invoke sequence outliner] hit kMaxTypeRefs");
      return false;
    }
    return true;
  }

  void insert_type_refs(const std::unordered_set<DexType*>& types) {
    always_assert(can_insert_type_refs(types));
    m_type_refs.insert(types.begin(), types.end());
    always_assert(m_type_refs.size() < kMaxTypeRefs);
  }

  bool can_insert_method_ref() {
    if (m_method_refs_count >= kMaxMethodRefs) {
      m_mgr.incr_metric("kMaxMethodRefs", 1);
      TRACE(ISO, 2, "[invoke sequence outliner] hit kMaxMethodRefs");
      return false;
    }
    return true;
  }

  void insert_method_ref() {
    always_assert(can_insert_method_ref());
    m_method_refs_count++;
    always_assert(m_method_refs_count <= kMaxMethodRefs);
  }

  // insert at beginning of dex, but after canary class, if any
  void insert_outlined_class(DexClass* outlined_cls) {
    auto it = m_dex.begin();
    for (; it != m_dex.end() &&
           (interdex::is_canary(*it) || is_outlined_class(*it));
         it++) {
    }
    m_dex.insert(it, outlined_cls);
  }

  // Class ids represent the position of a class in the dex; we use this to
  // determine if class in the dex, which one comes first, when deciding
  // on a host class for an outlined method.
  boost::optional<size_t> get_class_id(DexType* t) {
    auto it = m_class_ids.find(t);
    return it == m_class_ids.end() ? boost::none
                                   : boost::optional<size_t>(it->second);
  }
};

// Provides facilities to select existing, or create new host classes for
// outlined methods
class HostClassSelector {
 private:
  const InstructionSequenceOutlinerConfig& m_config;
  PassManager& m_mgr;
  DexState& m_dex_state;
  DexClass* m_outlined_cls{nullptr};
  size_t m_outlined_classes{0};
  size_t m_hosted_direct_count{0};
  size_t m_hosted_base_count{0};
  size_t m_hosted_helper_count{0};

 public:
  HostClassSelector() = delete;
  HostClassSelector(const HostClassSelector&) = delete;
  HostClassSelector& operator=(const HostClassSelector&) = delete;
  HostClassSelector(const InstructionSequenceOutlinerConfig& config,
                    PassManager& mgr,
                    DexState& dex_state)
      : m_config(config), m_mgr(mgr), m_dex_state(dex_state) {}
  ~HostClassSelector() {
    m_mgr.incr_metric("num_hosted_direct_count", m_hosted_direct_count);
    m_mgr.incr_metric("num_hosted_base_count", m_hosted_base_count);
    m_mgr.incr_metric("num_hosted_helper_count", m_hosted_helper_count);
    TRACE(ISO, 2,
          "[invoke sequence outliner] %zu direct, %zu base, %zu helpers hosted",
          m_hosted_direct_count, m_hosted_base_count, m_hosted_helper_count);

    m_mgr.incr_metric("num_outlined_classes", m_outlined_classes);
    TRACE(ISO, 2,
          "[invoke sequence outliner] %zu outlined helper classes created",
          m_outlined_classes);
  }

  // Return current outlined helper class, if exists and we can add one more
  // method to it
  DexType* reuse_last_outlined_class() {
    if (!m_outlined_cls || m_outlined_cls->get_dmethods().size() >=
                               m_config.max_outlined_methods_per_class) {
      return nullptr;
    }
    return m_outlined_cls->get_type();
  }

  // Create type that will represent next outlined helper class
  DexType* peek_at_next_outlined_class() {
    auto name =
        DexString::make_string(std::string(OUTLINED_CLASS_NAME_PREFIX) +
                               std::to_string(m_dex_state.get_dex_id()) + "$" +
                               std::to_string(m_outlined_classes) + ";");
    return DexType::make_type(name);
  }

  // Create a new helper class into which we can place outlined methods.
  void create_next_outlined_class() {
    always_assert(reuse_last_outlined_class() == nullptr);
    auto outlined_type = peek_at_next_outlined_class();
    m_outlined_classes++;
    ClassCreator cc(outlined_type);
    cc.set_access(ACC_PUBLIC | ACC_FINAL);
    cc.set_super(type::java_lang_Object());
    m_outlined_cls = cc.create();
    m_outlined_cls->rstate.set_generated();
    m_dex_state.insert_outlined_class(m_outlined_cls);
  }

  DexType* get_direct_or_base_class(const CandidateSequence& cs,
                                    const CandidateInfo& ci,
                                    bool* not_outlinable) {
    *not_outlinable = false;
    // When all candidate sequences come from methods of a single class, use
    // that type as the host class
    std::unordered_set<DexType*> types;
    for (auto& p : ci.methods) {
      types.insert(p.first->get_class());
    }
    always_assert(!types.empty());
    if (types.size() == 1) {
      auto direct_type = *types.begin();
      auto direct_cls = type_class(direct_type);
      if (direct_cls && can_rename(direct_cls) && can_delete(direct_cls)) {
        m_hosted_direct_count++;
        return *types.begin();
      }
      if (has_non_init_invoke_directs(cs)) {
        // TODO: Consider making those methods static if they can be renamed,
        // just like what the inliner does
        *not_outlinable = true;
        return (DexType*)nullptr;
      }
    }
    always_assert(!has_non_init_invoke_directs(cs));

    // When all candidates come from class with a common base type, use that.
    std::unordered_map<DexType*, size_t> expanded_types;
    for (auto t : types) {
      while (t != nullptr) {
        expanded_types[t]++;
        auto cls = type_class(t);
        if (!cls) {
          break;
        }
        t = cls->get_super_class();
      }
    }
    DexType* host_class{nullptr};
    boost::optional<size_t> host_class_id;
    for (auto& p : expanded_types) {
      if (p.second != types.size()) {
        continue;
      }
      auto class_id = m_dex_state.get_class_id(p.first);
      if (!class_id) {
        continue;
      }
      auto cls = type_class(p.first);
      if (!cls || !can_rename(cls) || !can_delete(cls)) {
        continue;
      }
      // In particular, use the base type that appears first in this dex.
      if (host_class == nullptr || *host_class_id > *class_id) {
        host_class_id = *class_id;
        host_class = p.first;
      }
    }
    if (host_class) {
      m_hosted_base_count++;
      return host_class;
    }

    // Fallback: put the outlined method in a dedicated helper class.
    m_hosted_helper_count++;
    return (DexType*)nullptr;
  }
};

// Outlining all occurrences of a particular candidate sequence.
bool outline_candidate(const CandidateSequence& cs,
                       const CandidateInfo& ci,
                       ReusableOutlinedMethods* reusable_outlined_methods,
                       DexState* dex_state,
                       HostClassSelector* host_class_selector,
                       OutlinedMethodCreator* outlined_method_creator) {
  // Before attempting to create or reuse an outlined method that hasn't been
  // referenced in this dex before, we'll make sure that all the involved
  // type refs can be added to the dex. We collect those type refs.
  std::unordered_set<DexType*> type_refs_to_insert;
  for (auto t : cs.arg_types) {
    type_refs_to_insert.insert(const_cast<DexType*>(t));
  }
  auto rtype = cs.res ? cs.res->type : type::_void();
  type_refs_to_insert.insert(const_cast<DexType*>(rtype));

  bool can_reuse{false};
  DexMethod* outlined_method;
  if (reusable_outlined_methods && reusable_outlined_methods->count(cs)) {
    outlined_method = reusable_outlined_methods->at(cs);
    type_refs_to_insert.insert(outlined_method->get_class());
    if (!dex_state->can_insert_type_refs(type_refs_to_insert)) {
      return false;
    }
  } else {
    bool not_outlinable;
    auto host_class =
        host_class_selector->get_direct_or_base_class(cs, ci, &not_outlinable);
    if (not_outlinable) {
      return false;
    }
    bool must_create_next_outlined_class{false};
    if (host_class == nullptr) {
      host_class = host_class_selector->reuse_last_outlined_class();
      if (host_class == nullptr) {
        host_class = host_class_selector->peek_at_next_outlined_class();
        must_create_next_outlined_class = true;
      }
      can_reuse = true;
    }
    type_refs_to_insert.insert(host_class);
    if (!dex_state->can_insert_type_refs(type_refs_to_insert)) {
      return false;
    }
    if (must_create_next_outlined_class) {
      host_class_selector->create_next_outlined_class();
    }
    outlined_method =
        outlined_method_creator->create_outlined_method(cs, host_class);
  }
  dex_state->insert_type_refs(type_refs_to_insert);
  for (auto& p : ci.methods) {
    auto method = p.first;
    auto& cfg = method->get_code()->cfg();
    for (auto& cml : p.second) {
      rewrite_sequence_at_location(outlined_method, cfg, cs, cml);
    }
    TRACE(ISO, 6, "[invoke sequence outliner] outlined from %s\n%s",
          SHOW(method), SHOW(cfg));
  }
  if (can_reuse && reusable_outlined_methods) {
    // The newly created outlined method was placed in a new helper class
    // which should be accessible without problems from later dexes
    reusable_outlined_methods->emplace(cs, outlined_method);
  }
  return true;
}

// Perform outlining of most beneficial candidates, while staying within
// reference limits.
static void outline(
    const InstructionSequenceOutlinerConfig& config,
    PassManager& mgr,
    DexState& dex_state,
    std::vector<Candidate>* candidates,
    std::unordered_map<DexMethod*, std::unordered_set<CandidateId>>*
        candidate_ids_by_methods,
    ReusableOutlinedMethods* reusable_outlined_methods) {
  MethodNameGenerator method_name_generator(mgr);
  OutlinedMethodCreator outlined_method_creator(mgr, method_name_generator);
  HostClassSelector host_class_selector(config, mgr, dex_state);
  // While we have a set of beneficial candidates, many are overlapping each
  // other. We are using a priority queue to iteratively outline the most
  // beneficial candidate at any point in time, then removing all impacted
  // other overlapping occurrences, which in turn changes the priority of
  // impacted candidates, until there is no more beneficial candidate left.
  using Priority = uint64_t;
  MutablePriorityQueue<CandidateId, Priority> pq;
  auto get_priority = [&config, &candidates,
                       reusable_outlined_methods](CandidateId id) {
    auto& c = candidates->at(id);
    Priority primary_priority =
        get_savings(config, c.sequence, c.info, reusable_outlined_methods) *
        c.sequence.size;
    // clip primary_priority to 32-bit
    if (primary_priority >= (1UL << 32)) {
      primary_priority = (1UL << 32) - 1;
    }
    // make unique via candidate id
    return (primary_priority << 32) | id;
  };
  auto erase = [&pq, candidate_ids_by_methods](CandidateId id, Candidate& c) {
    pq.erase(id);
    for (auto& p : c.info.methods) {
      (*candidate_ids_by_methods)[p.first].erase(id);
    }
    c.info.methods.clear();
    c.info.count = 0;
  };
  for (CandidateId id = 0; id < candidates->size(); id++) {
    pq.insert(id, get_priority(id));
  }
  size_t total_savings{0};
  size_t outlined_count{0};
  size_t outlined_sequences_count{0};
  size_t not_outlined_count{0};
  while (!pq.empty()) {
    // Make sure beforehand that there's a method ref left for us
    if (!dex_state.can_insert_method_ref()) {
      break;
    }

    auto id = pq.front();
    auto& c = candidates->at(id);
    auto savings =
        get_savings(config, c.sequence, c.info, reusable_outlined_methods);
    always_assert(savings > 0);
    total_savings += savings;
    outlined_count += c.info.count;
    outlined_sequences_count++;

    TRACE(ISO, 3,
          "[invoke sequence outliner] %4ux(%3u) [%zu]: %zu byte savings",
          c.info.count, c.info.methods.size(), c.sequence.size, 2 * savings);
    if (outline_candidate(c.sequence, c.info, reusable_outlined_methods,
                          &dex_state, &host_class_selector,
                          &outlined_method_creator)) {
      dex_state.insert_method_ref();
    } else {
      TRACE(ISO, 3, "[invoke sequence outliner] could not ouline");
      not_outlined_count++;
    }

    // Remove overlapping occurrences
    std::unordered_set<CandidateId> other_candidate_ids_with_changes;
    for (auto& p : c.info.methods) {
      auto method = p.first;
      auto& cmls = p.second;
      for (auto other_id : candidate_ids_by_methods->at(method)) {
        if (other_id == id) {
          continue;
        }
        auto& other_c = candidates->at(other_id);
        for (auto& cml : cmls) {
          auto start = cml.first_insn_idx;
          auto end = start + c.sequence.insns.size();
          auto& other_cmls = other_c.info.methods.at(method);
          for (auto it = other_cmls.begin(); it != other_cmls.end();) {
            auto& other_cml = *it;
            auto other_start = other_cml.first_insn_idx;
            auto other_end = other_start + other_c.sequence.insns.size();
            if (end > other_start && start < other_end) {
              it = other_cmls.erase(it);
              other_c.info.count--;
              if (other_id != id) {
                other_candidate_ids_with_changes.insert(other_id);
              }
            } else {
              ++it;
            }
          }
        }
      }
    }
    erase(id, c);
    // Update priorities of affected candidates
    for (auto other_id : other_candidate_ids_with_changes) {
      auto& other_c = candidates->at(other_id);
      auto other_savings = get_savings(config, other_c.sequence, other_c.info,
                                       reusable_outlined_methods);
      if (other_savings == 0) {
        erase(other_id, other_c);
      } else {
        pq.update_priority(other_id, get_priority(other_id));
      }
    }
  }

  mgr.incr_metric("num_not_outlined", not_outlined_count);
  TRACE(ISO, 2, "[invoke sequence outliner] %zu not outlined",
        not_outlined_count);

  mgr.incr_metric("num_outlined", outlined_count);
  mgr.incr_metric("num_outlined_sequences", outlined_sequences_count);
  mgr.incr_metric("num_total_savings", total_savings);
  TRACE(ISO, 1,
        "[invoke sequence outliner] %zu unique sequences outlined in %zu "
        "places; %zu total savings",
        outlined_sequences_count, outlined_count, total_savings);
}

////////////////////////////////////////////////////////////////////////////////
// clear_cfgs
////////////////////////////////////////////////////////////////////////////////

static void clear_cfgs(
    const Scope& scope,
    const std::unordered_map<std::string, unsigned int>* method_to_weight) {
  walk::parallel::code(
      scope, [&method_to_weight](DexMethod* method, IRCode& code) {
        if (!can_outline_from_method(method, method_to_weight)) {
          return;
        }
        code.clear_cfg();
      });
}

} // namespace

bool is_outlined_class(DexClass* cls) {
  const char* cname = cls->get_type()->get_name()->c_str();
  return strncmp(cname, OUTLINED_CLASS_NAME_PREFIX,
                 sizeof(OUTLINED_CLASS_NAME_PREFIX) - 1) == 0;
}

void InstructionSequenceOutliner::bind_config() {
  bind("max_insns_size", m_config.min_insns_size, m_config.min_insns_size,
       "Minimum number of instructions to be outlined in a sequence");
  bind("max_insns_size", m_config.max_insns_size, m_config.max_insns_size,
       "Maximum number of instructions to be outlined in a sequence");
  bind("use_method_to_weight", m_config.use_method_to_weight,
       m_config.use_method_to_weight,
       "Whether to use provided method-to-weight configuration data to "
       "determine if a method should not be outlined from");
  bind("reuse_outlined_methods_across_dexes",
       m_config.reuse_outlined_methods_across_dexes,
       m_config.reuse_outlined_methods_across_dexes,
       "Whether to allow reusing outlined methods across dexes within the same "
       "store");
  bind("max_outlined_methods_per_class",
       m_config.max_outlined_methods_per_class,
       m_config.max_outlined_methods_per_class,
       "Maximum number of outlined methods per generated helper class; "
       "indirectly drives number of needed helper classes");
  bind("threshold", m_config.threshold, m_config.threshold,
       "Minimum number of code units saved before a particular code sequence "
       "is outlined anywhere");
  always_assert(m_config.min_insns_size >= MIN_INSNS_SIZE);
  always_assert(m_config.max_insns_size >= m_config.min_insns_size);
  always_assert(m_config.max_outlined_methods_per_class > 0);
}

void InstructionSequenceOutliner::run_pass(DexStoresVector& stores,
                                           ConfigFiles& config,
                                           PassManager& mgr) {
  const std::unordered_map<std::string, unsigned int>* method_to_weight =
      m_config.use_method_to_weight ? &config.get_method_to_weight() : nullptr;
  XStoreRefs xstores(stores);
  size_t dex_id{0};
  const auto& interdex_metrics = mgr.get_interdex_metrics();
  auto it = interdex_metrics.find(interdex::METRIC_RESERVED_MREFS);
  size_t reserved_mrefs = it == interdex_metrics.end() ? 0 : it->second;
  TRACE(ISO, 2, "[invoke sequence outliner] found %zu reserved mrefs",
        reserved_mrefs);
  std::unique_ptr<ReusableOutlinedMethods> reusable_outlined_methods;
  if (m_config.reuse_outlined_methods_across_dexes) {
    reusable_outlined_methods = std::make_unique<ReusableOutlinedMethods>();
  }
  boost::optional<size_t> last_store_idx;
  for (auto& store : stores) {
    for (auto& dex : store.get_dexen()) {
      if (dex.empty()) {
        continue;
      }
      auto store_idx = xstores.get_store_idx(dex.front()->get_type());
      always_assert(std::find_if(dex.begin(), dex.end(),
                                 [&xstores, store_idx](DexClass* cls) {
                                   return xstores.get_store_idx(
                                              cls->get_type()) != store_idx;
                                 }) == dex.end());
      if (reusable_outlined_methods && last_store_idx &&
          xstores.illegal_ref_between_stores(store_idx, *last_store_idx)) {
        // TODO: Keep around all store dependencies and reuse when possible
        TRACE(ISO, 3,
              "Clearing reusable outlined methods when transitioning from "
              "store %zu to %zu",
              *last_store_idx, store_idx);
        reusable_outlined_methods->clear();
      }
      last_store_idx = store_idx;
      auto illegal_ref = [&xstores, store_idx](const DexType* t) {
        // TODO: Check if references to external classes that only exist
        // on some Android versions are problematic as well.
        return xstores.illegal_ref(store_idx, t);
      };
      CandidateInstructionCoresSet recurring_cores;
      get_recurring_cores(mgr, dex, method_to_weight, illegal_ref,
                          &recurring_cores);
      std::vector<Candidate> candidates;
      std::unordered_map<DexMethod*, std::unordered_set<CandidateId>>
          candidate_ids_by_methods;
      get_beneficial_candidates(m_config, mgr, dex, method_to_weight,
                                illegal_ref, recurring_cores,
                                reusable_outlined_methods.get(), &candidates,
                                &candidate_ids_by_methods);

      // TODO: Merge candidates that are equivalent except that one returns
      // something and the other doesn't.
      DexState dex_state(mgr, dex, dex_id++, reserved_mrefs);
      outline(m_config, mgr, dex_state, &candidates, &candidate_ids_by_methods,
              reusable_outlined_methods.get());
      clear_cfgs(dex, method_to_weight);
    }
  }
}

static InstructionSequenceOutliner s_pass;
