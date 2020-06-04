/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass outlines common instruction sequences within a basic block,
 * and across tree-shaped control-flow structures for size wins. The notion of
 * instruction sequence equivalence is modulo register names.
 *
 * At its core is a rather naive approach: check if any subsequence of
 * instructions in a block occurs sufficiently often. The average complexity is
 * held down by filtering out instruction sequences where adjacent sequences of
 * abstracted instructions ("cores") of fixed lengths never occur twice anywhere
 * in the scope (seems good enough, even without a suffix tree).
 *
 * When reaching a conditional branch or switch instruction, different control-
 * paths are explored as well, as long as they eventually all arrive at a common
 * block. Thus, outline candidates are in fact instruction sequence trees.
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
 * - Outline beyond control-flow trees, e.g. DAGs, or even arbitrary
 *   control-flow with try/catches
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
  size_t hash = cic.opcode;
  boost::hash_combine(hash, cic.literal);
  return hash;
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

struct CandidateEdgeLabel {
  cfg::EdgeType type;
  cfg::Edge::MaybeCaseKey case_key;
};
bool operator==(const CandidateEdgeLabel& a, const CandidateEdgeLabel& b) {
  return a.type == b.type && a.case_key == b.case_key;
}
bool operator!=(const CandidateEdgeLabel& a, const CandidateEdgeLabel& b) {
  return !(a == b);
}

struct CandidateNode {
  std::vector<CandidateInstruction> insns;
  boost::optional<reg_t> res_reg;
  std::vector<std::pair<CandidateEdgeLabel, std::shared_ptr<CandidateNode>>>
      succs;
};
static size_t hash_value(const CandidateNode& cn) {
  size_t hash = cn.insns.size();
  if (cn.res_reg) {
    boost::hash_combine(hash, *cn.res_reg);
  }
  boost::hash_combine(hash,
                      boost::hash_range(cn.insns.begin(), cn.insns.end()));
  for (auto& p : cn.succs) {
    boost::hash_combine(hash, *p.second);
  }
  return hash;
}
static StableHash stable_hash_value(const CandidateNode& cn) {
  StableHash stable_hash{cn.insns.size()};
  for (const auto& csi : cn.insns) {
    stable_hash = stable_hash * 73 + stable_hash_value(csi);
  }
  if (cn.res_reg) {
    stable_hash = stable_hash * 79 + *cn.res_reg;
  }
  for (auto& p : cn.succs) {
    stable_hash = stable_hash * 199 + stable_hash_value(*p.second);
  }
  return stable_hash;
}

using CandidateNodeHasher = boost::hash<CandidateNode>;
bool operator!=(const CandidateNode& a, const CandidateNode& b);
bool operator==(const CandidateNode& a, const CandidateNode& b) {
  if (a.insns != b.insns || a.res_reg != b.res_reg ||
      a.succs.size() != b.succs.size()) {
    return false;
  }
  for (size_t i = 0; i < a.succs.size(); i++) {
    auto& p = a.succs.at(i);
    auto& q = b.succs.at(i);
    if (p.first != q.first || *p.second != *q.second) {
      return false;
    }
  }
  return true;
}
bool operator!=(const CandidateNode& a, const CandidateNode& b) {
  return !(a == b);
}

struct Candidate {
  std::vector<const DexType*> arg_types;
  CandidateNode root;
  const DexType* res_type{nullptr};
  size_t size;
  reg_t temp_regs;
};
static size_t hash_value(const Candidate& c) {
  size_t hash = c.size;
  boost::hash_combine(hash, c.root);
  for (auto arg_type : c.arg_types) {
    boost::hash_combine(hash, (size_t)arg_type);
  }
  return hash;
}
static StableHash stable_hash_value(const Candidate& c) {
  StableHash stable_hash{c.arg_types.size()};
  for (auto t : c.arg_types) {
    stable_hash = stable_hash * 71 + stable_hash_value(show(t));
  }
  stable_hash = stable_hash * 73 + stable_hash_value(c.root);
  return stable_hash;
}

using CandidateHasher = boost::hash<Candidate>;
bool operator==(const Candidate& a, const Candidate& b) {
  if (a.arg_types != b.arg_types || a.root != b.root) {
    return false;
  }

  always_assert(a.size == b.size);
  always_assert(a.temp_regs == b.temp_regs);
  always_assert(a.res_type == b.res_type);
  return true;
}

static CandidateInstructionCore to_core(IRInstruction* insn) {
  CandidateInstructionCore core;
  core.opcode = insn->opcode();
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
  // When a value is only assigned along some control-flow paths.
  CONDITIONAL,
  // When we don't know whether an incoming object reference has been
  // initialized (could be addressed by another analysis, but not worth it)
  UNKNOWN,
  // A newly created object on which no constructor was invoked yet
  UNINITIALIZED,
  // A primitive value, array, or object on which a constructor was invoked
  INITIALIZED,
};

struct PartialCandidateNode {
  std::vector<IRInstruction*> insns;
  std::unordered_map<reg_t, RegState> defined_regs;
  std::vector<std::pair<cfg::Edge*, std::shared_ptr<PartialCandidateNode>>>
      succs;
};

// A partial candidate is still evolving, and defined against actual
// instructions that have not been normalized yet.
struct PartialCandidate {
  std::unordered_set<reg_t> in_regs;
  PartialCandidateNode root;
  // Total number of all instructions
  size_t insns_size{0};
  // Approximate number of code units occupied by all instructions
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
                                       const PartialCandidate& pc,
                                       reg_t reg) {
  const auto& env = type_environments->at(pc.root.insns.front());
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
                                      const PartialCandidate& pc,
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
    return get_initial_type(type_environments, pc, src);
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
static void get_type_demand_helper(
    DexMethod* method,
    LazyTypeEnvironments& type_environments,
    const PartialCandidateNode& pcn,
    std::unordered_set<reg_t> regs_to_track,
    std::unordered_set<const DexType*>* type_demands) {
  for (size_t insn_idx = 0;
       insn_idx < pcn.insns.size() && !regs_to_track.empty();
       insn_idx++) {
    auto insn = pcn.insns.at(insn_idx);
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
        type_demands->insert(
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
  for (auto& p : pcn.succs) {
    get_type_demand_helper(method, type_environments, *p.second, regs_to_track,
                           type_demands);
  }
}

static const DexType* get_type_demand(DexMethod* method,
                                      LazyTypeEnvironments& type_environments,
                                      const PartialCandidate& pc,
                                      reg_t reg) {
  std::unordered_set<const DexType*> type_demands;
  std::unordered_set<reg_t> regs_to_track{reg};
  get_type_demand_helper(method, type_environments, pc.root, regs_to_track,
                         &type_demands);
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
  return get_initial_type(type_environments, pc, reg);
}

static std::vector<cfg::Edge*> get_ordered_goto_and_branch_succs(
    cfg::Block* block) {
  std::vector<cfg::Edge*> succs =
      block->cfg().get_succ_edges_if(block, [](cfg::Edge* e) {
        return e->type() == cfg::EDGE_GOTO || e->type() == cfg::EDGE_BRANCH;
      });
  std::sort(succs.begin(), succs.end(), [](cfg::Edge* a, cfg::Edge* b) {
    if (a->type() != b->type()) {
      return a->type() < b->type();
    }
    return a->case_key() && *a->case_key() < *b->case_key();
  });
  return succs;
}

static CandidateEdgeLabel normalize(cfg::Edge* e) {
  return CandidateEdgeLabel{e->type(), e->case_key()};
}

// This method turns a tree of actual instructions into a normalized
// candidate instruction tree. The main purpose of normalization is to
// determine a canonical register assignment. Normalization also identifies the
// list and types of incoming arguments. Normalized temporary registers start
// at zero, and normalized argument registers follow after temporary registers
// in the order in which they are referenced by the instructions when walking
// the tree in order.
static Candidate normalize(DexMethod* method,
                           LazyTypeEnvironments& type_environments,
                           const PartialCandidate& pc,
                           const boost::optional<reg_t>& out_reg) {
  std::unordered_map<reg_t, reg_t> map;
  reg_t next_arg{pc.temp_regs};
  reg_t next_temp{0};
  Candidate c;
  c.size = pc.size;
  c.temp_regs = pc.temp_regs;
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
  // We keep track of temp registers only needed along some
  // (conditional) control-flow paths.
  using UndoMap = std::unordered_map<reg_t, boost::optional<reg_t>>;
  auto normalize_def = [&map, &next_temp](reg_t reg, bool wide,
                                          UndoMap* undo_map) {
    if (!undo_map->count(reg)) {
      auto it = map.find(reg);
      undo_map->emplace(reg, it == map.end()
                                 ? boost::none
                                 : boost::optional<reg_t>(it->second));
    }
    reg_t mapped_reg = next_temp;
    next_temp += wide ? 2 : 1;
    map[reg] = mapped_reg;
    return mapped_reg;
  };
  std::function<void(const PartialCandidateNode&, CandidateNode* cn,
                     std::vector<IRInstruction*> linear_insns)>
      walk;
  std::unordered_set<const DexType*> res_types;
  walk = [&type_environments, &pc, &map, &normalize_use, &normalize_def,
          &out_reg, &res_types,
          &walk](const PartialCandidateNode& pcn, CandidateNode* cn,
                 std::vector<IRInstruction*> linear_insns) {
    UndoMap undo_map;
    for (auto insn : pcn.insns) {
      linear_insns.push_back(insn);
      CandidateInstruction ci;
      ci.core = to_core(insn);

      for (size_t i = 0; i < insn->srcs_size(); i++) {
        ci.srcs.push_back(normalize_use(insn->src(i), insn->src_is_wide(i)));
      }
      if (insn->has_dest()) {
        ci.dest = normalize_def(insn->dest(), insn->dest_is_wide(), &undo_map);
      }
      cn->insns.push_back(ci);
    }
    if (pcn.succs.empty()) {
      if (out_reg) {
        size_t out_insn_idx{linear_insns.size() - 1};
        while (!has_dest(linear_insns.at(out_insn_idx), *out_reg)) {
          out_insn_idx--;
        }
        res_types.insert(
            get_result_type(type_environments, pc, linear_insns, out_insn_idx));
        cn->res_reg = map.at(*out_reg);
      }
    } else {
      for (auto& p : pcn.succs) {
        auto succ_cn = std::make_shared<CandidateNode>();
        cn->succs.emplace_back(normalize(p.first), succ_cn);
        walk(*p.second, succ_cn.get(), linear_insns);
      }
    }
    for (auto& p : undo_map) {
      if (p.second) {
        map[p.first] = *p.second;
      } else {
        map.erase(p.first);
      }
    }
  };
  walk(pc.root, &c.root, {});
  always_assert(next_temp == pc.temp_regs);
  for (auto reg : arg_regs) {
    auto type = get_type_demand(method, type_environments, pc, reg);
    c.arg_types.push_back(type);
  }
  if (out_reg) {
    always_assert(!res_types.empty());
    c.res_type = res_types.size() == 1 ? *res_types.begin() : nullptr;
  }

  return c;
}

////////////////////////////////////////////////////////////////////////////////
// find_method_candidate_sequences
////////////////////////////////////////////////////////////////////////////////

// A non-empty (ordered) map of range start positions to range sizes.
using Ranges = std::map<size_t, size_t>;

struct CandidateMethodLocation {
  IRInstruction* first_insn;
  cfg::Block* hint_block;
  boost::optional<reg_t> out_reg;
  // We use a linear instruction indexing scheme within a method to identify
  // ranges, which we use later to invalidate other overlapping candidates
  // while incrementally processing the most beneficial candidates using a
  // priority queue.
  Ranges ranges;
};

static bool ranges_overlap(const Ranges& a, const Ranges& b) {
  auto range_begin = [](const Ranges::const_iterator& it) { return it->first; };
  auto range_end = [](const Ranges::const_iterator& it) {
    return it->first + it->second;
  };
  for (auto a_it = a.begin(), b_it = b.begin();
       a_it != a.end() && b_it != b.end();) {
    if (range_end(a_it) <= range_begin(b_it)) {
      a_it++;
    } else if (range_end(b_it) <= range_begin(a_it)) {
      b_it++;
    } else {
      return true;
    }
  }
  return false;
}

static bool can_outline_opcode(IROpcode opcode) {
  switch (opcode) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
  case OPCODE_GOTO:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_MOVE_EXCEPTION:
  case OPCODE_RETURN:
  case OPCODE_RETURN_OBJECT:
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN_WIDE:
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
static bool append_to_partial_candidate(IRInstruction* insn,
                                        PartialCandidate* pc,
                                        PartialCandidateNode* pcn) {
  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_DIRECT && method::is_init(insn->get_method())) {
    auto it = pcn->defined_regs.find(insn->src(0));
    if (it == pcn->defined_regs.end() || it->second == RegState::UNKNOWN) {
      return false;
    }
    it->second = RegState::INITIALIZED;
  }
  for (size_t i = 0; i < insn->srcs_size(); i++) {
    auto src = insn->src(i);
    if (!pcn->defined_regs.count(src)) {
      pc->in_regs.insert(src);
      if (insn->src_is_wide(i)) {
        pc->in_regs.insert(src + 1);
      }
      if (pc->in_regs.size() > MAX_ARGS) {
        return false;
      }
    }
  }
  if (insn->has_dest()) {
    RegState reg_state = RegState::INITIALIZED;
    if (insn->opcode() == OPCODE_MOVE_OBJECT) {
      auto it = pcn->defined_regs.find(insn->src(0));
      reg_state =
          it == pcn->defined_regs.end() ? RegState::UNKNOWN : it->second;
    } else if (opcode == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT) {
      always_assert(!pcn->insns.empty());
      auto last_opcode = pcn->insns.back()->opcode();
      if (last_opcode == OPCODE_NEW_INSTANCE) {
        reg_state = RegState::UNINITIALIZED;
      }
    }
    pcn->defined_regs[insn->dest()] = reg_state;
    pc->temp_regs += insn->dest_is_wide() ? 2 : 1;
  }
  pcn->insns.push_back(insn);
  pc->insns_size++;
  if (!opcode::is_move(opcode)) {
    // Moves are likely still eliminated by reg-alloc or other opts
    pc->size += insn->size();
  }
  return true;
}

bool legal_refs(const std::function<bool(const DexType*)>& illegal_ref,
                IRInstruction* insn) {
  std::vector<DexType*> types;
  insn->gather_types(types);
  for (const auto* t : types) {
    if (illegal_ref(t)) {
      return false;
    }
  }
  return true;
}

static bool can_outline_insn(
    const std::function<bool(const DexType*)>& illegal_ref,
    IRInstruction* insn) {
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
    if (!legal_refs(illegal_ref, insn)) {
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
    if (!legal_refs(illegal_ref, insn)) {
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
      if (!legal_refs(illegal_ref, insn)) {
        return false;
      }
    }
  }
  return true;
}

static bool is_uniquely_reached_via_pred(cfg::Block* block) {
  auto& pred_edges = block->preds();
  return pred_edges.size() == 1 && block != block->cfg().entry_block();
}

// Check if starting from the given iterator, we have a tree-shaped
// branching structure, and gather a common eventual target block where
// the leaf blocks would unconditionally go to.
static std::unordered_set<cfg::Block*> get_eventual_targets_after_outlining(
    cfg::Block* first_block, const cfg::InstructionIterator& it) {
  always_assert(is_conditional_branch(it->insn->opcode()) ||
                is_switch(it->insn->opcode()));
  auto get_targets =
      [first_block](
          cfg::Block* start_block) -> std::unordered_set<cfg::Block*> {
    // The target could be the start block itself, if we don't find any other
    // eligible targets, or the target(s) we find at the end of further
    // conditional control-flow.
    std::unordered_set<cfg::Block*> targets{start_block};
    if (is_uniquely_reached_via_pred(start_block)) {
      auto big_block = big_blocks::get_big_block(start_block);
      if (big_block && big_block->same_try(first_block)) {
        auto last_block = big_block->get_last_block();
        auto last_insn_it = last_block->get_last_insn();
        if (last_insn_it != last_block->end() &&
            (is_conditional_branch(last_insn_it->insn->opcode()) ||
             is_switch(last_insn_it->insn->opcode()))) {
          auto last_insn_cfg_it =
              last_block->to_cfg_instruction_iterator(last_insn_it);
          auto more_targets = get_eventual_targets_after_outlining(
              start_block, last_insn_cfg_it);
          targets.insert(more_targets.begin(), more_targets.end());
        } else {
          auto goes_to = last_block->goes_to();
          if (goes_to) {
            targets.insert(goes_to);
          }
        }
      }
    }
    return targets;
  };
  auto block = it.block();
  auto succs = get_ordered_goto_and_branch_succs(block);
  if (succs.empty()) {
    return std::unordered_set<cfg::Block*>();
  }
  // We start with the targets of the first successor, and narrow them down
  // with all other successor targets.
  auto succs_it = succs.begin();
  auto targets = get_targets((*succs_it)->target());
  for (succs_it++; succs_it != succs.end() && !targets.empty(); succs_it++) {
    auto other_targets = get_targets((*succs_it)->target());
    for (auto targets_it = targets.begin(); targets_it != targets.end();) {
      if (other_targets.count(*targets_it)) {
        targets_it++;
      } else {
        targets_it = targets.erase(targets_it);
      }
    }
  }
  return targets;
}

using MethodCandidates =
    std::unordered_map<Candidate,
                       std::vector<CandidateMethodLocation>,
                       CandidateHasher>;

// Callback invoked when either...
// - one more instruction was successfully appended to the partial candidate,
// and a point was reached that could potentially mark the end of an outlined
// instruction sequence (in this case, there is no next_block), or
// - the end of a converged fully explored conditional control-flow fork has
// been reached (in this case, the iterator is at the end, and next_block
// indicates the block at which the converged control-flow will continue).
using ExploredCallback =
    std::function<void(PartialCandidate& pc,
                       cfg::Block* first_block,
                       const big_blocks::InstructionIterator& it,
                       cfg::Block* next_block)>;

// Look for and add entire candidate sequences starting at a
// particular point in a big block.
// Result indicates whether the big block was successfully explored to the end.
static bool explore_candidates_from(
    const InstructionSequenceOutlinerConfig& config,
    const std::function<bool(const DexType*)>& illegal_ref,
    const CandidateInstructionCoresSet& recurring_cores,
    PartialCandidate* pc,
    PartialCandidateNode* pcn,
    big_blocks::InstructionIterator it,
    const big_blocks::InstructionIterator& end,
    const ExploredCallback* explored_callback = nullptr) {
  boost::optional<IROpcode> prev_opcode;
  CandidateInstructionCoresBuilder cores_builder;
  auto first_block = it.block();
  auto& cfg = first_block->cfg();
  for (; it != end; prev_opcode = it->insn->opcode(), it++) {
    if (pc->insns_size >= config.max_insns_size) {
      return false;
    }
    auto insn = it->insn;
    if (pcn->insns.size() + 1 < MIN_INSNS_SIZE &&
        !can_outline_insn(illegal_ref, insn)) {
      return false;
    }
    cores_builder.push_back(insn);
    if (cores_builder.has_value() &&
        !recurring_cores.count(cores_builder.get_value())) {
      return false;
    }
    if (!append_to_partial_candidate(insn, pc, pcn)) {
      return false;
    }
    if (is_conditional_branch(insn->opcode()) || is_switch(insn->opcode())) {
      // If the branching structure is such that there's a tree where all
      // leaves nodes unconditionally goto a common block, then we'll attempt
      // to gather a partial candidate tree.
      // TODO: Handle not just conditional trees, but DAGs or even arbitrary
      // control-flow.
      auto targets =
          get_eventual_targets_after_outlining(first_block, it.unwrap());
      always_assert(targets.size() <= 1);
      TRACE(ISO, 3, "[invoke sequence outliner] %zu eventual targets",
            targets.size());
      if (targets.size() != 1) {
        return false;
      }
      auto block = it.block();
      auto succs = get_ordered_goto_and_branch_succs(block);
      auto defined_regs = pcn->defined_regs;
      pcn->defined_regs.clear();
      auto next_block = *targets.begin();
      for (auto succ_edge : succs) {
        auto succ_pcn = std::make_shared<PartialCandidateNode>();
        pcn->succs.emplace_back(succ_edge, succ_pcn);
        succ_pcn->defined_regs = defined_regs;
        if (succ_edge->target() != next_block) {
          auto succ_big_block = big_blocks::get_big_block(succ_edge->target());
          always_assert(succ_big_block);
          always_assert(succ_big_block->same_try(first_block));
          always_assert(
              is_uniquely_reached_via_pred(succ_big_block->get_first_block()));
          auto succ_ii = big_blocks::InstructionIterable(*succ_big_block);
          if (!explore_candidates_from(config, illegal_ref, recurring_cores, pc,
                                       succ_pcn.get(), succ_ii.begin(),
                                       succ_ii.end())) {
            return false;
          }
        }
        for (auto& q : succ_pcn->defined_regs) {
          auto defined_regs_it = pcn->defined_regs.find(q.first);
          if (defined_regs_it == pcn->defined_regs.end()) {
            pcn->defined_regs.insert(q);
          } else if (q.second < defined_regs_it->second) {
            defined_regs_it->second = q.second;
          }
        }
      }
      for (auto& p : pcn->succs) {
        for (auto& q : pcn->defined_regs) {
          if (!p.second->defined_regs.count(q.first)) {
            q.second = RegState::CONDITIONAL;
          }
        }
      }
      if (explored_callback) {
        (*explored_callback)(*pc, first_block, end, next_block);
      }
      return true;
    }
    // We cannot consider partial candidate sequences when they are
    // missing their move-result piece
    if (insn->has_move_result_any() &&
        !cfg.move_result_of(it.unwrap()).is_end()) {
      continue;
    }
    // We prefer not to consider sequences ending in const instructions.
    // (Experiments have shown that outlining the overlapping candidate without
    // a trailing consts tends to lead to better results and a faster outliner.)
    if (insn->opcode() == OPCODE_CONST || insn->opcode() == OPCODE_CONST_WIDE ||
        (insn->opcode() == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT && prev_opcode &&
         is_const(*prev_opcode))) {
      continue;
    }
    if (explored_callback) {
      (*explored_callback)(*pc, first_block, it, /* next_block */ nullptr);
    }
  }
  return true;
}

#define STATS                                \
  FOR_EACH(live_out_to_throw_edge)           \
  FOR_EACH(live_out_multiple)                \
  FOR_EACH(live_out_initialized_unknown)     \
  FOR_EACH(live_out_initialized_conditional) \
  FOR_EACH(live_out_initialized_not)         \
  FOR_EACH(arg_type_not_computed)            \
  FOR_EACH(arg_type_illegal)                 \
  FOR_EACH(res_type_not_computed)            \
  FOR_EACH(res_type_illegal)                 \
  FOR_EACH(overlap)

struct FindCandidatesStats {
#define FOR_EACH(name) std::atomic<size_t> name{0};
  STATS
#undef FOR_EACH
};

// For a single method, identify possible beneficial outlinable candidates.
// For each candidate, gather information about where exactly in the
// given method it is located.
static MethodCandidates find_method_candidates(
    const InstructionSequenceOutlinerConfig& config,
    const std::function<bool(const DexType*)>& illegal_ref,
    DexMethod* method,
    cfg::ControlFlowGraph& cfg,
    const CandidateInstructionCoresSet& recurring_cores,
    FindCandidatesStats* stats) {
  MethodCandidates candidates;
  LivenessFixpointIterator liveness_fp_iter(cfg);
  liveness_fp_iter.run({});
  LazyUnorderedMap<cfg::Block*,
                   std::unordered_map<IRInstruction*, LivenessDomain>>
      live_outs([&liveness_fp_iter](cfg::Block* block) {
        std::unordered_map<IRInstruction*, LivenessDomain> res;
        auto live_out = liveness_fp_iter.get_live_out_vars_at(block);
        for (auto it = block->rbegin(); it != block->rend(); ++it) {
          if (it->type != MFLOW_OPCODE) {
            continue;
          }
          res.emplace(it->insn, live_out);
          liveness_fp_iter.analyze_instruction(it->insn, &live_out);
        }
        return res;
      });
  // Variables that flow into a throw block, if any
  LazyUnorderedMap<cfg::Block*, LivenessDomain> throw_live_out(
      [&cfg, &liveness_fp_iter](cfg::Block* block) {
        auto res = LivenessDomain::bottom();
        for (auto e : cfg.get_succ_edges_of_type(block, cfg::EDGE_THROW)) {
          res.join_with(liveness_fp_iter.get_live_in_vars_at(e->target()));
        }
        return res;
      });
  LazyTypeEnvironments type_environments([method, &cfg]() {
    type_inference::TypeInference type_inference(cfg);
    type_inference.run(method);
    return type_inference.get_type_environments();
  });
  auto big_blocks = big_blocks::get_big_blocks(cfg);
  // Along big blocks, we are assigning consecutive indices to instructions.
  // We'll use this to manage ranges of instructions that need to get
  // invalidated when overlapping ranges of insturctions are outlined.
  Lazy<std::unordered_map<const IRInstruction*, size_t>> insn_idxes(
      [&big_blocks]() {
        std::unordered_map<const IRInstruction*, size_t> res;
        for (auto& big_block : big_blocks) {
          for (const auto& mie : big_blocks::InstructionIterable(big_block)) {
            res.emplace(mie.insn, res.size());
          }
        }
        return res;
      });

  struct {
#define FOR_EACH(name) size_t name{0};
    STATS
#undef FOR_EACH
  } lstats;

  // We are visiting the instructions in this method in "big block" chunks:
  // - The big blocks cover all blocks.
  // - It is safe to do so as they all share the same throw-edges, and any
  //   outlined method invocation will be placed in the first block of the big
  //   block, with the appropriate throw edges.
  for (auto& big_block : big_blocks) {
    ExploredCallback explored_callback =
        [&](PartialCandidate& pc,
            cfg::Block* first_block,
            const big_blocks::InstructionIterator& it,
            cfg::Block* next_block) {
          // At this point, we can consider the current candidate for
          // normalization and outlining, adding it to the set of outlining
          // candidates for this method
          if (pc.insns_size < config.min_insns_size) {
            // Code is below minimum configured size
            return;
          }
          if (pc.size <= COST_INVOKE_WITHOUT_RESULT) {
            // Partial candidate is not longer than the replacement invoke
            // instruction would be
            return;
          }
          std::vector<std::pair<reg_t, IRInstruction*>> live_out_consts;
          boost::optional<reg_t> out_reg;
          if (!pc.root.defined_regs.empty()) {
            LivenessDomain live_out;
            if (next_block) {
              live_out = liveness_fp_iter.get_live_in_vars_at(next_block);
            } else {
              live_out = live_outs[it.block()].at(it->insn);
            }
            auto first_block_throw_live_out = throw_live_out[first_block];
            for (auto& p : pc.root.defined_regs) {
              if (first_block_throw_live_out.contains(p.first)) {
                TRACE(ISO, 4,
                      "[invoke sequence outliner] [bail out] Cannot return "
                      "value that's live-in to a throw edge");
                lstats.live_out_to_throw_edge++;
                return;
              }
              if (live_out.contains(p.first)) {
                if (out_reg) {
                  TRACE(ISO, 4,
                        "[invoke sequence outliner] [bail out] Cannot have "
                        "more than one out-reg");
                  lstats.live_out_multiple++;
                  return;
                }
                if (p.second == RegState::UNKNOWN) {
                  TRACE(ISO, 4,
                        "[invoke sequence outliner] [bail out] Cannot return "
                        "object with unknown initialization state");
                  lstats.live_out_initialized_unknown++;
                  return;
                }
                if (p.second == RegState::CONDITIONAL) {
                  TRACE(ISO, 4,
                        "[invoke sequence outliner] [bail out] Cannot return "
                        "conditionally initialized");
                  lstats.live_out_initialized_conditional++;
                  return;
                }
                if (p.second == RegState::UNINITIALIZED) {
                  TRACE(ISO, 4,
                        "[invoke sequence outliner] [bail out] Cannot return "
                        "uninitialized");
                  lstats.live_out_initialized_not++;
                  return;
                }
                always_assert(p.second == RegState::INITIALIZED);
                out_reg = p.first;
              }
            }
          }
          if (out_reg && pc.size <= COST_INVOKE_WITH_RESULT) {
            // Code to outline is not longer than the replacement invoke
            // instruction would be
            return;
          }
          auto c = normalize(method, type_environments, pc, out_reg);
          if (std::find(c.arg_types.begin(), c.arg_types.end(), nullptr) !=
              c.arg_types.end()) {
            TRACE(ISO, 4,
                  "[invoke sequence outliner] [bail out] Could not infer "
                  "argument type");
            lstats.arg_type_not_computed++;
            return;
          }
          if (std::find_if(c.arg_types.begin(), c.arg_types.end(),
                           illegal_ref) != c.arg_types.end()) {
            TRACE(ISO, 4,
                  "[invoke sequence outliner] [bail out] Illegal argument "
                  "type");
            lstats.arg_type_illegal++;
            return;
          }
          if (out_reg && c.res_type == nullptr) {
            TRACE(ISO, 4,
                  "[invoke sequence outliner] [bail out] Could not infer "
                  "result type");
            lstats.res_type_not_computed++;
            return;
          }
          if (out_reg && illegal_ref(c.res_type)) {
            TRACE(ISO, 4,
                  "[invoke sequence outliner] [bail out] Illegal result type");
            lstats.res_type_illegal++;
            return;
          }
          auto& cmls = candidates[c];
          std::map<size_t, size_t> ranges;
          std::function<void(const PartialCandidateNode&)> insert_ranges;
          insert_ranges = [&insert_ranges, &ranges,
                           &insn_idxes](const PartialCandidateNode& pcn) {
            if (!pcn.insns.empty()) {
              auto start = insn_idxes->at(pcn.insns.front());
              auto size = pcn.insns.size();
              ranges.emplace(start, size);
            }
            for (auto& p : pcn.succs) {
              insert_ranges(*p.second);
            }
          };
          insert_ranges(pc.root);
          if (std::find_if(cmls.begin(), cmls.end(),
                           [&ranges](const CandidateMethodLocation& cml) {
                             return ranges_overlap(cml.ranges, ranges);
                           }) != cmls.end()) {
            lstats.overlap++;
          } else {
            cmls.push_back((CandidateMethodLocation){
                pc.root.insns.front(), first_block, out_reg, ranges});
          }
        };
    auto ii = big_blocks::InstructionIterable(big_block);
    for (auto it = ii.begin(), end = ii.end(); it != end; it++) {
      if (opcode::is_move_result_any(it->insn->opcode())) {
        // We cannot start a sequence at a move-result-any instruction
        continue;
      }
      PartialCandidate pc;
      explore_candidates_from(config, illegal_ref, recurring_cores, &pc,
                              &pc.root, it, end, &explored_callback);
    }
  }

#define FOR_EACH(name) \
  if (lstats.name) stats->name += lstats.name;
  STATS
#undef FOR_EACH
  return candidates;
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
// instruction sequences must be comprised of shorter recurring ones.
static void get_recurring_cores(
    PassManager& mgr,
    const Scope& scope,
    const std::unordered_map<std::string, unsigned int>* method_to_weight,
    const std::function<bool(const DexType*)>& illegal_ref,
    CandidateInstructionCoresSet* recurring_cores) {
  ConcurrentMap<CandidateInstructionCores, size_t,
                CandidateInstructionCoresHasher>
      concurrent_cores;
  walk::parallel::code(
      scope, [illegal_ref, method_to_weight,
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
            if (!can_outline_insn(illegal_ref, insn)) {
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
    std::unordered_map<Candidate, DexMethod*, CandidateHasher>;

static size_t get_savings(
    const InstructionSequenceOutlinerConfig& config,
    const Candidate& c,
    const CandidateInfo& ci,
    const ReusableOutlinedMethods* reusable_outlined_methods) {
  size_t cost = c.size * ci.count;
  size_t outlined_cost =
      COST_METHOD_METADATA +
      (c.res_type ? COST_INVOKE_WITH_RESULT : COST_INVOKE_WITHOUT_RESULT) *
          ci.count;
  if (!reusable_outlined_methods || !reusable_outlined_methods->count(c)) {
    outlined_cost += COST_METHOD_BODY + c.size;
  }

  return (outlined_cost + config.threshold) < cost ? (cost - outlined_cost) : 0;
}

using CandidateId = uint32_t;
struct CandidateWithInfo {
  Candidate candidate;
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
    std::vector<CandidateWithInfo>* candidates_with_infos,
    std::unordered_map<DexMethod*, std::unordered_set<CandidateId>>*
        candidate_ids_by_methods) {
  ConcurrentMap<Candidate, CandidateInfo, CandidateHasher>
      concurrent_candidates;
  FindCandidatesStats stats;
  walk::parallel::code(scope, [&config, method_to_weight, &illegal_ref,
                               &recurring_cores, &concurrent_candidates,
                               &stats](DexMethod* method, IRCode& code) {
    if (!can_outline_from_method(method, method_to_weight)) {
      return;
    }
    for (auto& p :
         find_method_candidates(config, illegal_ref, method, code.cfg(),
                                recurring_cores, &stats)) {
      std::vector<CandidateMethodLocation>& cmls = p.second;
      concurrent_candidates.update(p.first,
                                   [method, &cmls](const Candidate&,
                                                   CandidateInfo& info,
                                                   bool /* exists */) {
                                     info.methods.emplace(method, cmls);
                                     info.count += cmls.size();
                                   });
    }
  });
#define FOR_EACH(name) mgr.incr_metric("num_candidate_" #name, stats.name);
  STATS
#undef FOR_EACH
  using CandidateSet = std::unordered_set<Candidate, CandidateHasher>;
  std::map<DexMethod*, CandidateSet, dexmethods_comparator>
      candidates_by_methods;
  size_t beneficial_count{0}, maleficial_count{0};
  for (auto& p : concurrent_candidates) {
    if (get_savings(config, p.first, p.second, reusable_outlined_methods) > 0) {
      beneficial_count += p.second.count;
      for (auto& q : p.second.methods) {
        candidates_by_methods[q.first].insert(p.first);
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
  std::unordered_map<Candidate, CandidateId, CandidateHasher> candidate_ids;
  for (auto& p : candidates_by_methods) {
    auto& method_candidate_ids = (*candidate_ids_by_methods)[p.first];
    std::vector<std::pair<CandidateMethodLocation, CandidateSet::iterator>>
        ordered;
    for (auto it = p.second.begin(); it != p.second.end(); it++) {
      auto& c = *it;
      auto id_it = candidate_ids.find(c);
      if (id_it != candidate_ids.end()) {
        method_candidate_ids.insert(id_it->second);
        continue;
      }
      const auto& ci = concurrent_candidates.at_unsafe(c);
      for (auto& cml : ci.methods.at(p.first)) {
        ordered.emplace_back(cml, it);
      }
    }
    std::sort(
        ordered.begin(), ordered.end(),
        [](const std::pair<CandidateMethodLocation, CandidateSet::iterator>& a,
           const std::pair<CandidateMethodLocation, CandidateSet::iterator>&
               b) {
          auto& a_ranges = a.first.ranges;
          auto& b_ranges = b.first.ranges;
          if (a_ranges.size() != b_ranges.size()) {
            return a_ranges.size() < b_ranges.size();
          }
          for (auto a_it = a_ranges.begin(), b_it = b_ranges.begin();
               a_it != a_ranges.end(); a_it++, b_it++) {
            if (a_it->first != b_it->first) {
              return a_it->first < b_it->first;
            }
            if (a_it->second != b_it->second) {
              return a_it->second < b_it->second;
            }
          }
          always_assert(a.first.first_insn == b.first.first_insn);
          always_assert(a.first.hint_block == b.first.hint_block);
          always_assert(a.second == b.second);
          return false;
        });
    for (auto& q : ordered) {
      auto& c = *q.second;
      if (!candidate_ids.count(c)) {
        always_assert(candidate_ids.size() < (1ULL << 32));
        CandidateId candidate_id = candidate_ids.size();
        method_candidate_ids.insert(candidate_id);
        candidate_ids.emplace(c, candidate_id);
        candidates_with_infos->push_back(
            {c, concurrent_candidates.at_unsafe(c)});
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// outline
////////////////////////////////////////////////////////////////////////////////

static bool has_non_init_invoke_directs(const CandidateNode& cn) {
  for (const auto& ci : cn.insns) {
    if (ci.core.opcode == OPCODE_INVOKE_DIRECT &&
        !method::is_init(ci.core.method)) {
      return true;
    }
  }
  for (auto& p : cn.succs) {
    if (has_non_init_invoke_directs(*p.second)) {
      return true;
    }
  }
  return false;
}

static bool has_non_init_invoke_directs(const Candidate& c) {
  return has_non_init_invoke_directs(c.root);
}

// A name generator for outlined methods
class MethodNameGenerator {
 private:
  PassManager& m_mgr;
  std::unordered_map<const DexType*, std::unordered_map<StableHash, size_t>>
      m_unique_method_ids;
  size_t m_max_unique_method_id{0};

 public:
  MethodNameGenerator() = delete;
  MethodNameGenerator(const MethodNameGenerator&) = delete;
  MethodNameGenerator& operator=(const MethodNameGenerator&) = delete;
  explicit MethodNameGenerator(PassManager& mgr) : m_mgr(mgr) {}

  // Compute the name of the outlined method in a way that tends to be stable
  // across Redex runs.
  DexString* get_name(const DexType* host_class, const Candidate& c) {
    StableHash stable_hash = stable_hash_value(c);
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
  size_t m_outlined_method_nodes{0};
  size_t m_outlined_method_positions{0};

  // The "best" representative set of debug position is that which provides
  // most detail, i.e. has the highest number of unique debug positions
  using PositionMap = std::map<const CandidateInstruction*, DexPosition*>;
  PositionMap get_best_outlined_dbg_positions(const Candidate& c,
                                              const CandidateInfo& ci) {
    PositionMap best_positions;
    size_t best_unique_positions{0};
    std::vector<DexMethod*> ordered_methods;
    for (auto& p : ci.methods) {
      ordered_methods.push_back(p.first);
    }
    std::sort(ordered_methods.begin(), ordered_methods.end(),
              compare_dexmethods);
    for (auto method : ordered_methods) {
      auto& cfg = method->get_code()->cfg();
      auto& cmls = ci.methods.at(method);
      for (auto& cml : cmls) {
        auto root_insn_it = cfg.find_insn(cml.first_insn, cml.hint_block);
        if (root_insn_it.is_end()) {
          // Shouldn't happen, but we are not going to fight that here.
          continue;
        }
        auto dbg_pos = cfg.get_dbg_pos(root_insn_it);
        if (!dbg_pos) {
          continue;
        }
        PositionMap positions;
        std::unordered_set<DexPosition*> unique_positions;
        std::function<void(DexPosition * dbg_pos, const CandidateNode& cn,
                           big_blocks::Iterator it)>
            walk;
        walk = [&positions, &unique_positions, &walk](DexPosition* last_dbg_pos,
                                                      const CandidateNode& cn,
                                                      big_blocks::Iterator it) {
          cfg::Block* last_block{nullptr};
          for (auto& csi : cn.insns) {
            auto next_dbg_pos{last_dbg_pos};
            for (; it->type == MFLOW_POSITION || it->type == MFLOW_DEBUG;
                 it++) {
              if (it->type == MFLOW_POSITION) {
                next_dbg_pos = it->pos.get();
              }
            }
            always_assert(it->type == MFLOW_OPCODE);
            always_assert(it->insn->opcode() == csi.core.opcode);
            positions.emplace(&csi, last_dbg_pos);
            unique_positions.insert(last_dbg_pos);
            last_dbg_pos = next_dbg_pos;
            last_block = it.block();
            it++;
          }
          if (!cn.succs.empty()) {
            always_assert(last_block != nullptr);
            auto succs = get_ordered_goto_and_branch_succs(last_block);
            always_assert(succs.size() == cn.succs.size());
            for (size_t i = 0; i < succs.size(); i++) {
              always_assert(normalize(succs.at(i)) == cn.succs.at(i).first);
              auto succ_cn = *cn.succs.at(i).second;
              auto succ_block = succs.at(i)->target();
              auto succ_it =
                  big_blocks::Iterator(succ_block, succ_block->begin());
              walk(last_dbg_pos, succ_cn, succ_it);
            }
          }
        };
        walk(dbg_pos, c.root,
             big_blocks::Iterator(root_insn_it.block(), root_insn_it.unwrap(),
                                  /* ignore_throws */ true));
        if (unique_positions.size() > best_unique_positions) {
          best_positions = positions;
          best_unique_positions = unique_positions.size();
          if (best_unique_positions == c.root.insns.size()) {
            break;
          }
        }
      }
    }
    return best_positions;
  }

  // Construct an IRCode datastructure from a candidate.
  std::unique_ptr<IRCode> get_outlined_code(DexMethod* outlined_method,
                                            const Candidate& c,
                                            const PositionMap& dbg_positions) {
    auto code = std::make_unique<IRCode>(outlined_method, c.temp_regs);
    std::function<void(const CandidateNode& cn)> walk;
    walk = [this, &code, &dbg_positions, &walk, &c](const CandidateNode& cn) {
      m_outlined_method_nodes++;
      DexPosition* last_dbg_pos{nullptr};
      for (auto& ci : cn.insns) {
        auto it = dbg_positions.find(&ci);
        if (it != dbg_positions.end()) {
          DexPosition* dbg_pos = it->second;
          if (dbg_pos != last_dbg_pos &&
              !opcode::is_move_result_pseudo(ci.core.opcode)) {
            auto cloned_dbg_pos = std::make_unique<DexPosition>(*dbg_pos);
            cloned_dbg_pos->parent = nullptr;
            code->push_back(std::move(cloned_dbg_pos));
            last_dbg_pos = dbg_pos;
            m_outlined_method_positions++;
          }
        }
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
      m_outlined_method_instructions += cn.insns.size();
      if (cn.succs.empty()) {
        if (c.res_type != nullptr) {
          IROpcode ret_opcode = type::is_object(c.res_type)
                                    ? OPCODE_RETURN_OBJECT
                                    : type::is_wide_type(c.res_type)
                                          ? OPCODE_RETURN_WIDE
                                          : OPCODE_RETURN;
          auto ret_insn = new IRInstruction(ret_opcode);
          ret_insn->set_src(0, *cn.res_reg);
          code->push_back(ret_insn);
        } else {
          auto ret_insn = new IRInstruction(OPCODE_RETURN_VOID);
          code->push_back(ret_insn);
        }
      } else {
        auto& last_mie = *code->rbegin();
        for (auto& p : cn.succs) {
          auto& e = p.first;
          if (e.type == cfg::EDGE_GOTO) {
            always_assert(e == cn.succs.front().first);
            code->push_back(); // MFLOW_FALLTHROUGH
          } else {
            always_assert(e.type == cfg::EDGE_BRANCH);
            BranchTarget* branch_target =
                e.case_key ? new BranchTarget(&last_mie, *e.case_key)
                           : new BranchTarget(&last_mie);
            code->push_back(branch_target);
          }
          walk(*p.second);
        }
        return;
      }
    };
    walk(c.root);
    return code;
  }

 public:
  OutlinedMethodCreator() = delete;
  OutlinedMethodCreator(const OutlinedMethodCreator&) = delete;
  OutlinedMethodCreator& operator=(const OutlinedMethodCreator&) = delete;
  explicit OutlinedMethodCreator(PassManager& mgr,
                                 MethodNameGenerator& method_name_generator)
      : m_mgr(mgr), m_method_name_generator(method_name_generator) {}

  // Obtain outlined method for a candidate.
  DexMethod* create_outlined_method(const Candidate& c,
                                    const CandidateInfo& ci,
                                    const DexType* host_class) {
    auto name = m_method_name_generator.get_name(host_class, c);
    std::deque<DexType*> arg_types;
    for (auto t : c.arg_types) {
      arg_types.push_back(const_cast<DexType*>(t));
    }
    auto rtype = c.res_type ? c.res_type : type::_void();
    auto type_list = DexTypeList::make_type_list(std::move(arg_types));
    auto proto = DexProto::make_proto(rtype, type_list);
    auto outlined_method =
        DexMethod::make_method(const_cast<DexType*>(host_class), name, proto)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    auto dbg_positions = get_best_outlined_dbg_positions(c, ci);
    outlined_method->set_code(
        get_outlined_code(outlined_method, c, dbg_positions));
    outlined_method->set_deobfuscated_name(show(outlined_method));
    outlined_method->rstate.set_dont_inline();
    change_visibility(outlined_method->get_code(),
                      const_cast<DexType*>(host_class), outlined_method);
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
    m_mgr.incr_metric("num_outlined_method_nodes", m_outlined_method_nodes);
    m_mgr.incr_metric("num_outlined_method_positions",
                      m_outlined_method_positions);
    TRACE(ISO, 2,
          "[invoke sequence outliner] %zu outlined methods with %zu "
          "instructions across %zu nodes and %zu positions",
          m_outlined_methods, m_outlined_method_instructions,
          m_outlined_method_nodes, m_outlined_method_positions);
  }
};

// Rewrite instructions in existing method to invoke an outlined
// method instead.
static void rewrite_at_location(DexMethod* outlined_method,
                                cfg::ControlFlowGraph& cfg,
                                const Candidate& c,
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
  const auto first_arg_reg = c.temp_regs;
  boost::optional<reg_t> last_arg_reg;
  std::vector<reg_t> arg_regs;
  std::function<void(const CandidateNode& cn,
                     big_blocks::InstructionIterator it)>
      walk;
  walk = [&walk, first_arg_reg, &last_arg_reg, &arg_regs, &cfg_mutation](
             const CandidateNode& cn, big_blocks::InstructionIterator it) {
    cfg::Block* last_block{nullptr};
    for (size_t insn_idx = 0; insn_idx < cn.insns.size();
         last_block = it.block(), insn_idx++, it++) {
      auto& ci = cn.insns.at(insn_idx);
      always_assert(it->insn->opcode() == ci.core.opcode);
      for (size_t i = 0; i < ci.srcs.size(); i++) {
        auto mapped_reg = ci.srcs.at(i);
        if (mapped_reg >= first_arg_reg &&
            (!last_arg_reg || mapped_reg > *last_arg_reg)) {
          last_arg_reg = mapped_reg;
          arg_regs.push_back(it->insn->src(i));
        }
      }
      if (!opcode::is_move_result_any(it->insn->opcode())) {
        cfg_mutation.remove(it.unwrap());
      }
    }
    if (!cn.succs.empty()) {
      always_assert(last_block != nullptr);
      auto succs = get_ordered_goto_and_branch_succs(last_block);
      always_assert(succs.size() == cn.succs.size());
      for (size_t i = 0; i < succs.size(); i++) {
        always_assert(normalize(succs.at(i)) == cn.succs.at(i).first);
        auto succ_cn = *cn.succs.at(i).second;
        auto succ_block = succs.at(i)->target();
        auto succ_ii = InstructionIterable(succ_block);
        auto succ_it = big_blocks::InstructionIterator(
            succ_block->to_cfg_instruction_iterator(succ_ii.begin()));
        walk(succ_cn, succ_it);
      }
    }
  };
  walk(c.root, big_blocks::InstructionIterator(first_insn_it,
                                               /* ignore_throws */ true));

  // Generate and insert invocation instructions
  std::vector<IRInstruction*> outlined_method_invocation;
  IRInstruction* invoke_insn = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke_insn->set_method(outlined_method);
  invoke_insn->set_srcs_size(arg_regs.size());
  for (size_t i = 0; i < arg_regs.size(); i++) {
    invoke_insn->set_src(i, arg_regs.at(i));
  }
  outlined_method_invocation.push_back(invoke_insn);
  if (c.res_type) {
    IRInstruction* move_result_insn =
        new IRInstruction(opcode::move_result_for_invoke(outlined_method));
    move_result_insn->set_dest(*cml.out_reg);
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
  std::unordered_set<const DexType*> m_type_refs;
  size_t m_method_refs_count;
  std::unordered_map<const DexType*, size_t> m_class_ids;

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

  bool can_insert_type_refs(const std::unordered_set<const DexType*>& types) {
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

  void insert_type_refs(const std::unordered_set<const DexType*>& types) {
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
  boost::optional<size_t> get_class_id(const DexType* t) {
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
  size_t m_hosted_at_refs_count{0};
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
    m_mgr.incr_metric("num_hosted_at_refs_count", m_hosted_at_refs_count);
    m_mgr.incr_metric("num_hosted_helper_count", m_hosted_helper_count);
    TRACE(ISO, 2,
          "[invoke sequence outliner] %zu direct, %zu base, %zu at refs, %zu "
          "helpers hosted",
          m_hosted_direct_count, m_hosted_base_count, m_hosted_at_refs_count,
          m_hosted_helper_count);

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

  const DexType* get_direct_or_base_class(
      const std::unordered_set<const DexType*>& types,
      const std::function<bool(const DexType*)>& predicate =
          [](const DexType*) { return true; }) {
    // When there's only one type, try to use that
    if (types.size() == 1) {
      auto direct_type = *types.begin();
      if (m_dex_state.get_class_id(direct_type) && predicate(direct_type)) {
        auto direct_cls = type_class(direct_type);
        always_assert(direct_cls);
        if (can_rename(direct_cls) && can_delete(direct_cls)) {
          return direct_type;
        }
      }
    }

    // Try to find the first common base class in this dex
    std::unordered_map<const DexType*, size_t> expanded_types;
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
    const DexType* host_class{nullptr};
    boost::optional<size_t> host_class_id;
    for (auto& p : expanded_types) {
      if (p.second != types.size()) {
        continue;
      }
      auto class_id = m_dex_state.get_class_id(p.first);
      if (!class_id || !predicate(p.first)) {
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
    return host_class;
  }

  const DexType* get_direct_or_base_class(const Candidate& c,
                                          const CandidateInfo& ci,
                                          bool* not_outlinable) {
    *not_outlinable = false;
    // When all candidate instances come from methods of a single class, use
    // that type as the host class
    std::unordered_set<const DexType*> types;
    for (auto& p : ci.methods) {
      types.insert(p.first->get_class());
    }
    always_assert(!types.empty());

    auto host_class = get_direct_or_base_class(types);
    if (types.size() == 1) {
      auto direct_type = *types.begin();
      if (host_class == direct_type) {
        m_hosted_direct_count++;
        return host_class;
      }
      if (has_non_init_invoke_directs(c)) {
        // TODO: Consider making those methods static if they can be renamed,
        // just like what the inliner does
        *not_outlinable = true;
        return nullptr;
      };
    }
    always_assert(!has_non_init_invoke_directs(c));

    if (host_class) {
      m_hosted_base_count++;
      return host_class;
    }

    types.clear();
    // If an outlined code snippet occurs in many unrelated classes, but always
    // references the same types (which share a common base type), then that
    // common base type is a reasonable place where to put the outlined code.
    auto insert_internal_type = [&types](const DexType* t) {
      auto cls = type_class(t);
      if (cls && !cls->is_external()) {
        types.insert(t);
      }
    };
    if (c.res_type) {
      insert_internal_type(c.res_type);
    }
    for (auto t : c.arg_types) {
      insert_internal_type(t);
    }
    std::function<void(const CandidateNode& cn)> walk;
    walk = [&insert_internal_type, &walk](const CandidateNode& cn) {
      for (auto& csi : cn.insns) {
        auto& cic = csi.core;
        switch (opcode::ref(cic.opcode)) {
        case opcode::Ref::Method:
          insert_internal_type(cic.method->get_class());
          break;
        case opcode::Ref::Field:
          insert_internal_type(cic.field->get_class());
          break;
        case opcode::Ref::Type:
          insert_internal_type(cic.type);
          break;
        default:
          break;
        }
      }
      for (auto& p : cn.succs) {
        walk(*p.second);
      }
    };
    walk(c.root);

    host_class = get_direct_or_base_class(types, [](const DexType* t) {
      // We don't want any common base class with a scary clinit
      auto cl_init = type_class(t)->get_clinit();
      return !cl_init ||
             (can_delete(cl_init) && cl_init->rstate.no_optimizations());
    });
    if (host_class) {
      m_hosted_at_refs_count++;
      return host_class;
    }

    // Fallback: put the outlined method in a dedicated helper class.
    m_hosted_helper_count++;
    return nullptr;
  }
};

// Outlining all occurrences of a particular candidate.
bool outline_candidate(const Candidate& c,
                       const CandidateInfo& ci,
                       ReusableOutlinedMethods* reusable_outlined_methods,
                       DexState* dex_state,
                       HostClassSelector* host_class_selector,
                       OutlinedMethodCreator* outlined_method_creator) {
  // Before attempting to create or reuse an outlined method that hasn't been
  // referenced in this dex before, we'll make sure that all the involved
  // type refs can be added to the dex. We collect those type refs.
  std::unordered_set<const DexType*> type_refs_to_insert;
  for (auto t : c.arg_types) {
    type_refs_to_insert.insert(const_cast<DexType*>(t));
  }
  auto rtype = c.res_type ? c.res_type : type::_void();
  type_refs_to_insert.insert(const_cast<DexType*>(rtype));

  bool can_reuse{false};
  DexMethod* outlined_method;
  if (reusable_outlined_methods && reusable_outlined_methods->count(c)) {
    outlined_method = reusable_outlined_methods->at(c);
    type_refs_to_insert.insert(outlined_method->get_class());
    if (!dex_state->can_insert_type_refs(type_refs_to_insert)) {
      return false;
    }
    TRACE(ISO, 5, "[invoke sequence outliner] reused %s",
          SHOW(outlined_method));
  } else {
    bool not_outlinable;
    auto host_class =
        host_class_selector->get_direct_or_base_class(c, ci, &not_outlinable);
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
        outlined_method_creator->create_outlined_method(c, ci, host_class);
  }
  dex_state->insert_type_refs(type_refs_to_insert);
  for (auto& p : ci.methods) {
    auto method = p.first;
    auto& cfg = method->get_code()->cfg();
    TRACE(ISO, 7, "[invoke sequence outliner] before outlined %s from %s\n%s",
          SHOW(outlined_method), SHOW(method), SHOW(cfg));
    for (auto& cml : p.second) {
      rewrite_at_location(outlined_method, cfg, c, cml);
    }
    TRACE(ISO, 6, "[invoke sequence outliner] after outlined %s from %s\n%s",
          SHOW(outlined_method), SHOW(method), SHOW(cfg));
  }
  if (can_reuse && reusable_outlined_methods) {
    // The newly created outlined method was placed in a new helper class
    // which should be accessible without problems from later dexes
    reusable_outlined_methods->emplace(c, outlined_method);
  }
  return true;
}

// Perform outlining of most beneficial candidates, while staying within
// reference limits.
static void outline(
    const InstructionSequenceOutlinerConfig& config,
    PassManager& mgr,
    DexState& dex_state,
    std::vector<CandidateWithInfo>* candidates_with_infos,
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
  auto get_priority = [&config, &candidates_with_infos,
                       reusable_outlined_methods](CandidateId id) {
    auto& cwi = candidates_with_infos->at(id);
    Priority primary_priority = get_savings(config, cwi.candidate, cwi.info,
                                            reusable_outlined_methods) *
                                cwi.candidate.size;
    // clip primary_priority to 32-bit
    if (primary_priority >= (1UL << 32)) {
      primary_priority = (1UL << 32) - 1;
    }
    // make unique via candidate id
    return (primary_priority << 32) | id;
  };
  auto erase = [&pq, candidate_ids_by_methods](CandidateId id,
                                               CandidateWithInfo& cwi) {
    pq.erase(id);
    for (auto& p : cwi.info.methods) {
      (*candidate_ids_by_methods)[p.first].erase(id);
    }
    cwi.info.methods.clear();
    cwi.info.count = 0;
  };
  for (CandidateId id = 0; id < candidates_with_infos->size(); id++) {
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
    auto& cwi = candidates_with_infos->at(id);
    auto savings =
        get_savings(config, cwi.candidate, cwi.info, reusable_outlined_methods);
    always_assert(savings > 0);
    total_savings += savings;
    outlined_count += cwi.info.count;
    outlined_sequences_count++;

    TRACE(ISO, 3,
          "[invoke sequence outliner] %4ux(%3u) [%zu]: %zu byte savings",
          cwi.info.count, cwi.info.methods.size(), cwi.candidate.size,
          2 * savings);
    if (outline_candidate(cwi.candidate, cwi.info, reusable_outlined_methods,
                          &dex_state, &host_class_selector,
                          &outlined_method_creator)) {
      dex_state.insert_method_ref();
    } else {
      TRACE(ISO, 3, "[invoke sequence outliner] could not ouline");
      not_outlined_count++;
    }

    // Remove overlapping occurrences
    std::unordered_set<CandidateId> other_candidate_ids_with_changes;
    for (auto& p : cwi.info.methods) {
      auto method = p.first;
      auto& cmls = p.second;
      for (auto other_id : candidate_ids_by_methods->at(method)) {
        if (other_id == id) {
          continue;
        }
        auto& other_c = candidates_with_infos->at(other_id);
        for (auto& cml : cmls) {
          auto& other_cmls = other_c.info.methods.at(method);
          for (auto it = other_cmls.begin(); it != other_cmls.end();) {
            auto& other_cml = *it;
            if (ranges_overlap(cml.ranges, other_cml.ranges)) {
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
    erase(id, cwi);
    // Update priorities of affected candidates
    for (auto other_id : other_candidate_ids_with_changes) {
      auto& other_cwi = candidates_with_infos->at(other_id);
      auto other_savings =
          get_savings(config, other_cwi.candidate, other_cwi.info,
                      reusable_outlined_methods);
      if (other_savings == 0) {
        erase(other_id, other_cwi);
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
                 strlen(OUTLINED_CLASS_NAME_PREFIX)) == 0;
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
      std::vector<CandidateWithInfo> candidates_with_infos;
      std::unordered_map<DexMethod*, std::unordered_set<CandidateId>>
          candidate_ids_by_methods;
      get_beneficial_candidates(
          m_config, mgr, dex, method_to_weight, illegal_ref, recurring_cores,
          reusable_outlined_methods.get(), &candidates_with_infos,
          &candidate_ids_by_methods);

      // TODO: Merge candidates that are equivalent except that one returns
      // something and the other doesn't.
      DexState dex_state(mgr, dex, dex_id++, reserved_mrefs);
      outline(m_config, mgr, dex_state, &candidates_with_infos,
              &candidate_ids_by_methods, reusable_outlined_methods.get());
      clear_cfgs(dex, method_to_weight);
    }
  }
}

static InstructionSequenceOutliner s_pass;
