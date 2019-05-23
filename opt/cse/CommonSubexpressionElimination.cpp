/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This optimizer pass eliminates common subexpression.
 *
 * It's implemented via a global-value-numbering scheme.
 * While doing abstract interpretation on a method's code, we evolve...
 * 1) a mapping of registers to "values"
 * 2) a mapping of "values" to first-defining instructions
 *
 * A "value" is similar to an instruction, in that it has an IROpcode,
 * a list of srcs dependencies, and type/field/string/... payload data as
 * necessary; however it's different in that it doesn't have an identity, and
 * srcs dependencies are expressed in terms of other values, not registers.
 *
 * If the same value has multiple (equivalent) defining instructions after the
 * analysis reaches its fixed point, then the optimization...
 * - inserts a move of the result to a temporary register after the
 *   defining instruction, and it
 * - inserts another move from the temporary register to the result register
 *   of later (equivalent) defining instruction, after the defining instruction
 *
 * The moves are usually eliminated by copy-propagation, and the now redundant
 * later defining instructions are removed by local dce --- both of which get to
 * run on a method's code immediately if cse did a mutation.
 *
 * Notes:
 * - Memory read instructions are captured as well, and, in effect, may be
 *   reordered --- basically, later redundant reads may be replaced by results
 *   of earlier reads.
 *   Of course, true memory barriers are modeled (method invocations, volatile
 *   field accesses, monitor instructions), and to be conservative, all other
 *   writes to the heap (fields, array elements) are also treated as a memory
 *   barrier. This certainly ensures that thread-local behaviors is unaffected.
 * - There is no proper notion of phi-nodes at this time. Instead, conflicting
 *   information in the register-to-values and values'-first-definitions envs
 *   simply merge to top. Similarly, (memory) barriers are realized by setting
 *   all barrier-sensitive (heap-dependent) mapping entries to top. When later
 *   an instruction is interpreted that depends on a source register where the
 *   register-to-value binding is top, then a special value is created for that
 *   register (a "pre-state-source" value that refers to the value of a source
 *   register as it was *before* the instruction). This recovers the tracking
 *   of merged or havoced registers, in a way that's similar to phi-nodes, but
 *   lazy.
 *
 * Future work:
 * - Implement proper phi-nodes, tracking merged values as early as possible,
 *   instead of just tracking on first use after value went to 'top'. Not sure
 *   if there are tangible benefits.
 * - Making tracking of (memory) barrier more refined. As far as I understand
 *   the memory model, reads can be freely reordered in the absence of
 *   true synchronization events, and to maintain thread-local behavior we
 *   could further partition the "barrier-sensitive" store by tracking
 *   separately...
 *   - different fields accessed
 *   - different array element kinds accessed
 * - Identify pure method invocations that do not need to trigger a (memory)
 *   barrier
 *
 */

#include "CommonSubexpressionElimination.h"

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "CopyPropagationPass.h"
#include "HashedSetAbstractDomain.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "LocalDce.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

using namespace sparta;

namespace {

constexpr const char* METRIC_RESULTS_CAPTURED = "num_results_captured";
constexpr const char* METRIC_ELIMINATED_INSTRUCTIONS =
    "num_eliminated_instructions";

using value_id_t = uint32_t;
enum ValueIdFlags : value_id_t {
  IS_PRE_STATE_SRC = 0x01,
  IS_BARRIER_SENSITIVE = 0x02,
  BASE = 0x04,
};

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

// Marker opcode for values representing a source of an instruction; this is
// used to recover from merged / havoced values.
const IROpcode IOPCODE_PRE_STATE_SRC = IROpcode(0xFFFF);

struct IRValue {
  IROpcode opcode;
  std::vector<value_id_t> srcs;
  union {
    // Zero-initialize this union with the uint64_t member instead of a
    // pointer-type member so that it works properly even on 32-bit machines
    uint64_t literal{0};
    const DexString* string;
    const DexType* type;
    const DexFieldRef* field;
    const DexMethodRef* method;
    const DexOpcodeData* data;

    // By setting positional_insn to the pointer of an instruction, it
    // effectively makes the "value" unique (as unique as the instruction),
    // avoiding identifying otherwise structurally equivalent operations, e.g.
    // two move-exception instructions that really must remain at their existing
    // position, and cannot be replaced.
    const IRInstruction* positional_insn;
  };
};

struct IRValueHasher {
  size_t operator()(const IRValue& tv) const {
    size_t hash = tv.opcode;
    for (auto src : tv.srcs) {
      hash = hash * 27 + src;
    }
    hash = hash * 27 + (size_t)tv.literal;
    return hash;
  }
};

bool operator==(const IRValue& a, const IRValue& b) {
  return a.opcode == b.opcode && a.srcs == b.srcs && a.literal == b.literal;
}

using IRInstructionDomain = sparta::ConstantAbstractDomain<IRInstruction*>;
using ValueIdDomain = sparta::ConstantAbstractDomain<value_id_t>;
using DefEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<value_id_t, IRInstructionDomain>;
using RefEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<register_t, ValueIdDomain>;

class CseEnvironment final
    : public sparta::ReducedProductAbstractDomain<CseEnvironment,
                                                  DefEnvironment,
                                                  DefEnvironment,
                                                  RefEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;
  CseEnvironment() = default;

  CseEnvironment(std::initializer_list<std::pair<register_t, ValueIdDomain>>)
      : ReducedProductAbstractDomain(std::make_tuple(
            DefEnvironment(), DefEnvironment(), RefEnvironment())) {}

  static void reduce_product(
      std::tuple<DefEnvironment, DefEnvironment, RefEnvironment>&) {}

  const DefEnvironment& get_def_env(bool is_barrier_sensitive) const {
    if (is_barrier_sensitive) {
      return ReducedProductAbstractDomain::get<0>();
    } else {
      return ReducedProductAbstractDomain::get<1>();
    }
  }

  const RefEnvironment& get_ref_env() const {
    return ReducedProductAbstractDomain::get<2>();
  }

  CseEnvironment& mutate_def_env(bool is_barrier_sensitive,
                                 std::function<void(DefEnvironment*)> f) {
    if (is_barrier_sensitive) {
      apply<0>(f);
    } else {
      apply<1>(f);
    }
    return *this;
  }

  CseEnvironment& mutate_ref_env(std::function<void(RefEnvironment*)> f) {
    apply<2>(f);
    return *this;
  }
};

static CommonSubexpressionElimination::Barrier make_barrier(
    const IRInstruction* insn) {
  CommonSubexpressionElimination::Barrier b;
  b.opcode = insn->opcode();
  if (insn->has_field()) {
    b.field = resolve_field(insn->get_field(), is_sfield_op(insn->opcode())
                                                   ? FieldSearch::Static
                                                   : FieldSearch::Instance);
  } else if (insn->has_method()) {
    b.method = resolve_method(insn->get_method(), opcode_to_search(insn));
  }
  return b;
}

class Analyzer final : public BaseIRAnalyzer<CseEnvironment> {
 public:
  Analyzer(CommonSubexpressionElimination::SharedState* shared_state,
           cfg::ControlFlowGraph& cfg)
      : BaseIRAnalyzer(cfg),
        m_read_accesses(shared_state->compute_read_accesses(cfg)),
        m_shared_state(shared_state) {
    MonotonicFixpointIterator::run(CseEnvironment::top());
  }

  void analyze_instruction(IRInstruction* insn,
                           CseEnvironment* current_state) const override {

    const auto set_current_state_at = [&](register_t reg, bool wide,
                                          ValueIdDomain value) {
      current_state->mutate_ref_env([&](RefEnvironment* env) {
        env->set(reg, value);
        if (wide) {
          env->set(reg + 1, ValueIdDomain::top());
        }
      });
    };

    init_pre_state(insn, current_state);
    auto opcode = insn->opcode();
    switch (opcode) {
    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_WIDE: {
      auto domain = current_state->get_ref_env().get(insn->src(0));
      set_current_state_at(insn->dest(), insn->dest_is_wide(), domain);
      break;
    }
    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT:
    case OPCODE_MOVE_RESULT_WIDE:
    case IOPCODE_MOVE_RESULT_PSEUDO:
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case IOPCODE_MOVE_RESULT_PSEUDO_WIDE: {
      auto domain = current_state->get_ref_env().get(RESULT_REGISTER);
      auto c = domain.get_constant();
      if (c) {
        auto value_id = *c;
        auto ibs = is_barrier_sensitive(value_id);
        if (!current_state->get_def_env(ibs).get(value_id).get_constant()) {
          current_state->mutate_def_env(ibs, [&](DefEnvironment* env) {
            env->set(value_id, IRInstructionDomain(insn));
          });
        }
      }
      set_current_state_at(insn->dest(), insn->dest_is_wide(), domain);
      break;
    }
    default: {
      // If we get here, reset destination.
      if (insn->dests_size()) {
        ValueIdDomain domain = get_value_id_domain(insn, current_state);
        auto c = domain.get_constant();
        if (c) {
          auto value_id = *c;
          auto ibs = is_barrier_sensitive(value_id);
          if (!current_state->get_def_env(ibs).get(value_id).get_constant()) {
            current_state->mutate_def_env(ibs, [&](DefEnvironment* env) {
              env->set(value_id, IRInstructionDomain(insn));
            });
          }
          domain = ValueIdDomain(value_id);
        }
        set_current_state_at(insn->dest(), insn->dest_is_wide(), domain);
      } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
        ValueIdDomain domain = get_value_id_domain(insn, current_state);
        current_state->mutate_ref_env(
            [&](RefEnvironment* env) { env->set(RESULT_REGISTER, domain); });
      }
      break;
    }
    }

    if (m_shared_state->is_barrier(insn, m_read_accesses)) {
      auto b = make_barrier(insn);
      m_shared_state->log_barrier(b);

      // TODO: This is quite conservative and can be relaxed:
      // - the only real barriers are volatile field accesses, monitor
      //   instructions, and invocations of un-analyzable methods
      // - for analyzable methods we could compute some kind of summary
      // - for non-volatile heap writes, we could keep track of some type
      //   information or even alias information, and only reset that portion
      //   of the def-env which is actually affected

      current_state->mutate_def_env(true /* is_barrier_sensitive */,
                                    [](DefEnvironment* env) { env->clear(); });
      current_state->mutate_ref_env([&](RefEnvironment* env) {
        if (!env->is_value()) {
          return;
        }
        std::unordered_set<register_t> barrier_sensitive_regs;
        // TODO: The following loop is probably the most expensive thing in this
        // algorithm; is there a better way of doing this? (Then again, overall,
        // the time this algorithm takes seems reasonable.)
        for (auto& p : env->bindings()) {
          auto c = p.second.get_constant();
          if (c) {
            auto value_id = *c;
            if (is_barrier_sensitive(value_id)) {
              barrier_sensitive_regs.insert(p.first);
            }
          }
        }
        for (auto reg : barrier_sensitive_regs) {
          env->set(reg, ValueIdDomain::top());
        }
      });
    }
  }

  bool is_pre_state_src(value_id_t value_id) const {
    return !!(value_id & ValueIdFlags::IS_PRE_STATE_SRC);
  }

  bool is_barrier_sensitive(value_id_t value_id) const {
    return !!(value_id & ValueIdFlags::IS_BARRIER_SENSITIVE);
  }

 private:
  ValueIdDomain get_value_id_domain(const IRInstruction* insn,
                                    CseEnvironment* current_state) const {
    auto value = get_value(insn, current_state);
    auto value_id = get_value_id(value);
    return ValueIdDomain(value_id);
  }

  value_id_t get_pre_state_src_value_id(register_t reg,
                                        const IRInstruction* insn) const {
    auto value = get_pre_state_src_value(reg, insn);
    return get_value_id(value);
  }

  value_id_t get_value_id(const IRValue& value) const {
    auto it = m_value_ids.find(value);
    if (it != m_value_ids.end()) {
      return it->second;
    }
    value_id_t id = m_value_ids.size() * ValueIdFlags::BASE;
    always_assert(id / ValueIdFlags::BASE == m_value_ids.size());
    switch (value.opcode) {
    case OPCODE_IGET:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_WIDE:
    case OPCODE_IGET_SHORT:
    case OPCODE_IGET_OBJECT:
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_AGET:
    case OPCODE_AGET_BYTE:
    case OPCODE_AGET_CHAR:
    case OPCODE_AGET_WIDE:
    case OPCODE_AGET_SHORT:
    case OPCODE_AGET_OBJECT:
    case OPCODE_AGET_BOOLEAN:
    case OPCODE_SGET:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_WIDE:
    case OPCODE_SGET_SHORT:
    case OPCODE_SGET_OBJECT:
    case OPCODE_SGET_BOOLEAN:
      id |= ValueIdFlags::IS_BARRIER_SENSITIVE;
      break;
    case IOPCODE_PRE_STATE_SRC:
      id |= ValueIdFlags::IS_PRE_STATE_SRC;
      break;
    default:
      for (auto src : value.srcs) {
        if (is_barrier_sensitive(src)) {
          id |= ValueIdFlags::IS_BARRIER_SENSITIVE;
          break;
        }
      }
      break;
    }
    m_value_ids.emplace(value, id);
    return id;
  }

  IRValue get_pre_state_src_value(register_t reg,
                                  const IRInstruction* insn) const {
    IRValue value;
    value.opcode = IOPCODE_PRE_STATE_SRC;
    value.srcs.push_back(reg);
    value.positional_insn = insn;
    return value;
  }

  void init_pre_state(const IRInstruction* insn,
                      CseEnvironment* current_state) const {
    auto ref_env = current_state->get_ref_env();
    std::unordered_map<uint32_t, value_id_t> new_pre_state_src_values;
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      auto reg = insn->src(i);
      auto c = ref_env.get(reg).get_constant();
      if (!c) {
        auto it = new_pre_state_src_values.find(reg);
        if (it == new_pre_state_src_values.end()) {
          auto value_id = get_pre_state_src_value_id(reg, insn);
          new_pre_state_src_values.emplace(reg, value_id);
        }
      }
    }
    if (new_pre_state_src_values.size()) {
      current_state->mutate_ref_env([&](RefEnvironment* env) {
        for (auto& p : new_pre_state_src_values) {
          env->set(p.first, ValueIdDomain(p.second));
        }
      });
    }
  }

  IRValue get_value(const IRInstruction* insn,
                    CseEnvironment* current_state) const {
    IRValue value;
    auto opcode = insn->opcode();
    always_assert(opcode != IOPCODE_PRE_STATE_SRC);
    value.opcode = opcode;
    auto ref_env = current_state->get_ref_env();
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      auto reg = insn->src(i);
      auto c = ref_env.get(reg).get_constant();
      always_assert(c);
      value_id_t value_id = *c;
      value.srcs.push_back(value_id);
    }
    if (opcode::is_commutative(opcode)) {
      std::sort(value.srcs.begin(), value.srcs.end());
    }
    bool is_positional;
    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM_WIDE:
    case OPCODE_MOVE_EXCEPTION:
    case OPCODE_NEW_ARRAY:
    case OPCODE_NEW_INSTANCE:
    case OPCODE_FILLED_NEW_ARRAY:
      is_positional = true;
      break;
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_INTERFACE:
      // TODO: Identify pure methods; they do not need to be positional.
      is_positional = true;
      break;
    default:
      is_positional = m_shared_state->is_barrier(insn, m_read_accesses);
      break;
    }
    if (is_positional) {
      value.positional_insn = insn;
    } else if (insn->has_literal()) {
      value.literal = insn->get_literal();
    } else if (insn->has_type()) {
      value.type = insn->get_type();
    } else if (insn->has_field()) {
      value.field = insn->get_field();
    } else if (insn->has_method()) {
      value.method = insn->get_method();
    } else if (insn->has_string()) {
      value.string = insn->get_string();
    } else if (insn->has_data()) {
      value.data = insn->get_data();
    }
    return value;
  }

  CommonSubexpressionElimination::ReadAccesses m_read_accesses;
  CommonSubexpressionElimination::SharedState* m_shared_state;
  mutable std::unordered_map<IRValue, value_id_t, IRValueHasher> m_value_ids;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

CommonSubexpressionElimination::SharedState::SharedState() {
  // The following methods are...
  // - static, or
  // - direct (constructors), or
  // - virtual methods defined in final classes
  // that do not mutate any fields or array elements that could be directly
  // accessed (read or written) by user code, and they will not invoke user
  // code.
  //
  // The list of methods is not exhaustive; it was derived by observing the most
  // common barriers encountered in real-life code, and then studying their spec
  // to check whether they are "safe" in the context of CSE barriers.
  //
  // (Some items are commented out; that's because they are virtual methods in
  // non-final classes. They could be re-enabled once we can track the exact
  // receiver type.3)
  static const char* safe_method_names[] = {
      "Landroid/os/SystemClock;.elapsedRealtime:()J",
      "Landroid/os/SystemClock;.uptimeMillis:()J",
      "Landroid/util/Pair;.<init>:(Ljava/lang/Object;Ljava/lang/Object;)V",
      "Landroid/util/SparseArray;.<init>:()V",
      // "Landroid/util/SparseArray;.append:(ILjava/lang/Object;)V",
      // "Landroid/util/SparseArray;.get:(I)Ljava/lang/Object;",
      // "Landroid/util/SparseArray;.put:(ILjava/lang/Object;)V",
      // "Landroid/util/SparseArray;.size:()I",
      // "Landroid/util/SparseArray;.valueAt:(I)Ljava/lang/Object;",
      // "Landroid/util/SparseIntArray;.put:(II)V",
      "Ljava/io/IOException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Boolean;.booleanValue:()Z",
      "Ljava/lang/Boolean;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Boolean;.getBoolean:(Ljava/lang/String;)Z",
      "Ljava/lang/Boolean;.hashCode:()I",
      "Ljava/lang/Boolean;.parseBoolean:(Ljava/lang/String;)Z",
      "Ljava/lang/Boolean;.toString:()Ljava/lang/String;",
      "Ljava/lang/Boolean;.toString:(Z)Ljava/lang/String;",
      "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;",
      "Ljava/lang/Boolean;.valueOf:(Ljava/lang/String;)Ljava/lang/Boolean;",
      "Ljava/lang/Byte;.byteValue:()B",
      "Ljava/lang/Byte;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Byte;.parseByte:(Ljava/lang/String;)B",
      "Ljava/lang/Byte;.toString:()Ljava/lang/String;",
      "Ljava/lang/Byte;.toString:(B)Ljava/lang/String;",
      "Ljava/lang/Byte;.valueOf:(B)Ljava/lang/Byte;",
      "Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class;",
      "Ljava/lang/Class;.getName:()Ljava/lang/String;",
      "Ljava/lang/Class;.getSimpleName:()Ljava/lang/String;",
      "Ljava/lang/Double;.compare:(DD)I", "Ljava/lang/Double;.doubleValue:()D",
      "Ljava/lang/Double;.doubleToLongBits:(D)J",
      "Ljava/lang/Double;.doubleToRawLongBits:(D)J",
      "Ljava/lang/Double;.longBitsToDouble:(J)D",
      "Ljava/lang/Double;.parseDouble:(Ljava/lang/String;)D",
      "Ljava/lang/Double;.valueOf:(D)Ljava/lang/Double;",
      "Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V",
      "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Enum;.name:()Ljava/lang/String;",
      "Ljava/lang/Enum;.ordinal:()I",
      "Ljava/lang/Enum;.valueOf:(Ljava/lang/Class;Ljava/lang/String;)Ljava/"
      "lang/Enum;",
      "Ljava/lang/Exception;.<init>:()V", "Ljava/lang/Float;.floatValue:()F",
      "Ljava/lang/Float;.compare:(FF)I",
      "Ljava/lang/Float;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Float;.intBitsToFloat:(I)F",
      "Ljava/lang/Float;.floatToIntBits:(F)I",
      "Ljava/lang/Float;.isInfinite:(F)Z", "Ljava/lang/Float;.isNaN:(F)Z",
      "Ljava/lang/Float;.parseFloat:(Ljava/lang/String;)F",
      "Ljava/lang/Float;.valueOf:(F)Ljava/lang/Float;",
      "Ljava/lang/Float;.toString:(F)Ljava/lang/String;",
      "Ljava/lang/IllegalArgumentException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/IllegalStateException;.<init>:()V",
      "Ljava/lang/IllegalStateException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Integer;.<init>:(I)V", "Ljava/lang/Integer;.byteValue:()B",
      "Ljava/lang/Integer;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Integer;.hashCode:()I",
      "Ljava/lang/Integer;.highestOneBit:(I)I",
      "Ljava/lang/Integer;.intValue:()I", "Ljava/lang/Integer;.longValue:()J",
      "Ljava/lang/Integer;.parseInt:(Ljava/lang/String;)I",
      "Ljava/lang/Integer;.parseInt:(Ljava/lang/String;I)I",
      "Ljava/lang/Integer;.shortValue:()S",
      "Ljava/lang/Integer;.toBinaryString:(I)Ljava/lang/String;",
      "Ljava/lang/Integer;.toHexString:(I)Ljava/lang/String;",
      "Ljava/lang/Integer;.toString:(I)Ljava/lang/String;",
      "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;",
      "Ljava/lang/Integer;.valueOf:(Ljava/lang/String;)Ljava/lang/Integer;",
      "Ljava/lang/Long;.<init>:(J)V", "Ljava/lang/Long;.bitCount:(J)I",
      "Ljava/lang/Long;.compareTo:(Ljava/lang/Long;)I",
      "Ljava/lang/Long;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Long;.hashCode:()I", "Ljava/lang/Long;.intValue:()I",
      "Ljava/lang/Long;.longValue:()J",
      "Ljava/lang/Long;.parseLong:(Ljava/lang/String;)J",
      "Ljava/lang/Long;.signum:(J)I",
      "Ljava/lang/Long;.toBinaryString:(J)Ljava/lang/String;",
      "Ljava/lang/Long;.toHexString:(J)Ljava/lang/String;",
      "Ljava/lang/Long;.toString:()Ljava/lang/String;",
      "Ljava/lang/Long;.toString:(J)Ljava/lang/String;",
      "Ljava/lang/Long;.valueOf:(J)Ljava/lang/Long;",
      "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Object;.<init>:()V",
      "Ljava/lang/Object;.getClass:()Ljava/lang/Class;",
      "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Short;.<init>:(S)V",
      "Ljava/lang/Short;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/Short;.shortValue:()S",
      "Ljava/lang/Short;.toString:(S)Ljava/lang/String;",
      "Ljava/lang/Short;.valueOf:(S)Ljava/lang/Short;",
      "Ljava/lang/String;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/String;.charAt:(I)C",
      "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;",
      "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z",
      "Ljava/lang/String;.equalsIgnoreCase:(Ljava/lang/String;)Z",
      "Ljava/lang/String;.getBytes:()[B", "Ljava/lang/String;.hashCode:()I",
      "Ljava/lang/String;.indexOf:(I)I", "Ljava/lang/String;.isEmpty:()Z",
      "Ljava/lang/String;.lastIndexOf:(I)I",
      "Ljava/lang/String;.lastIndexOf:(I)I", "Ljava/lang/String;.length:()I",
      "Ljava/lang/String;.split:(Ljava/lang/String;)[Ljava/lang/String;",
      "Ljava/lang/String;.startsWith:(Ljava/lang/String;)Z",
      "Ljava/lang/String;.substring:(I)Ljava/lang/String;",
      "Ljava/lang/String;.substring:(II)Ljava/lang/String;",
      "Ljava/lang/String;.trim:()Ljava/lang/String;",
      "Ljava/lang/String;.valueOf:(I)Ljava/lang/String;",
      "Ljava/lang/String;.valueOf:(J)Ljava/lang/String;",
      "Ljava/lang/String;.valueOf:(Z)Ljava/lang/String;",
      "Ljava/lang/StringBuilder;.<init>:()V",
      "Ljava/lang/StringBuilder;.<init>:(I)V",
      "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/StringBuilder;.append:(C)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(I)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(J)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
      "StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(Z)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.length:()I",
      "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;",
      "Ljava/lang/System;.currentTimeMillis:()J",
      "Ljava/lang/System;.identityHashCode:(Ljava/lang/Object;)I",
      "Ljava/lang/System;.nanoTime:()J",
      "Ljava/lang/Thread;.currentThread:()Ljava/lang/Thread;",
      "Ljava/lang/UnsupportedOperationException;.<init>:(Ljava/lang/"
      "String;)V",
      "Ljava/util/ArrayList;.<init>:()V", "Ljava/util/ArrayList;.<init>:(I)V",
      // "Ljava/util/ArrayList;.add:(Ljava/lang/Object;)Z",
      // "Ljava/util/ArrayList;.clear:()V",
      // "Ljava/util/ArrayList;.get:(I)Ljava/lang/Object;",
      // "Ljava/util/ArrayList;.isEmpty:()Z",
      // "Ljava/util/ArrayList;.remove:(I)Ljava/lang/Object;",
      // "Ljava/util/ArrayList;.size:()I",
      "Ljava/util/BitSet;.<init>:(I)V",
      // "Ljava/util/BitSet;.set:(I)V",
      "Ljava/util/HashMap;.<init>:()V", "Ljava/util/HashSet;.<init>:()V",
      "Ljava/util/LinkedHashMap;.<init>:()V",
      "Ljava/util/LinkedList;.<init>:()V",
      // "Ljava/util/LinkedList;.size:()I",
      // "Ljava/util/List;.add:(Ljava/lang/Object;)Z",
      // "Ljava/util/List;.clear:()V",
      // "Ljava/util/List;.get:(I)Ljava/lang/Object;",
      // "Ljava/util/List;.isEmpty:()Z",
      // "Ljava/util/List;.iterator:()Ljava/util/Iterator;",
      // "Ljava/util/List;.remove:(I)Ljava/lang/Object;",
      // "Ljava/util/List;.size:()I",
      // "Ljava/util/Map;.clear:()V",
      // "Ljava/util/Map;.size:()I",
      // "Ljava/util/Map$Entry;.getKey:()Ljava/lang/Object;",
      // "Ljava/util/Map$Entry;.getValue:()Ljava/lang/Object;",
      // "Ljava/util/Set;.isEmpty:()Z",
      // "Ljava/util/Set;.iterator:()Ljava/util/Iterator;",
      // "Ljava/util/Set;.size:()I"
  };

  for (auto const safe_method_name : safe_method_names) {
    const std::string& s(safe_method_name);
    auto method = DexMethod::get_method(s);
    if (method == nullptr) {
      fprintf(stderr, "[CSE]: Could not find safe method %s\n", s.c_str());
    } else {
      m_safe_methods.insert(method);
    }
  }

  m_safe_types.insert(DexType::make_type("Ljava/lang/Math;"));
  if (traceEnabled(CSE, 2)) {
    m_barriers.reset(new ConcurrentMap<Barrier, size_t, BarrierHasher>());
  }
}

void CommonSubexpressionElimination::SharedState::init_method_barriers(
    const Scope& scope) {
  auto non_true_virtuals = devirtualize(scope);
  std::copy(non_true_virtuals.begin(), non_true_virtuals.end(),
            std::inserter(m_non_true_virtuals, m_non_true_virtuals.end()));

  ConcurrentMap<DexMethod*, std::vector<Barrier>> method_barriers;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    code.build_cfg(/* editable */ true);
    method_barriers.emplace(method, compute_barriers(code.cfg()));
  });
  std::copy(method_barriers.begin(), method_barriers.end(),
            std::inserter(m_method_barriers, m_method_barriers.end()));
}

CommonSubexpressionElimination::ReadAccesses
CommonSubexpressionElimination::SharedState::compute_read_accesses(
    cfg::ControlFlowGraph& cfg) {
  ReadAccesses ra;
  for (auto& mie : cfg::InstructionIterable(cfg)) {
    auto* insn = mie.insn;
    switch (insn->opcode()) {
    case OPCODE_AGET:
      ra.array_component_types[CommonSubexpressionElimination::INT] = true;
      break;
    case OPCODE_AGET_BYTE:
      ra.array_component_types[CommonSubexpressionElimination::BYTE] = true;
      break;
    case OPCODE_AGET_CHAR:
      ra.array_component_types[CommonSubexpressionElimination::CHAR] = true;
      break;
    case OPCODE_AGET_WIDE:
      ra.array_component_types[CommonSubexpressionElimination::WIDE] = true;
      break;
    case OPCODE_AGET_SHORT:
      ra.array_component_types[CommonSubexpressionElimination::SHORT] = true;
      break;
    case OPCODE_AGET_OBJECT:
      ra.array_component_types[CommonSubexpressionElimination::OBJECT] = true;
      break;
    case OPCODE_AGET_BOOLEAN:
      ra.array_component_types[CommonSubexpressionElimination::BOOLEAN] = true;
      break;
    default:
      if (is_iget(insn->opcode()) || is_sget(insn->opcode())) {
        auto field_ref = insn->get_field();
        DexField* field = resolve_field(field_ref, is_sfield_op(insn->opcode())
                                                       ? FieldSearch::Static
                                                       : FieldSearch::Instance);
        if (field == nullptr || !is_volatile(field)) {
          ra.fields.insert(field);
        }
      }
    }
  }
  return ra;
}

std::vector<CommonSubexpressionElimination::Barrier>
CommonSubexpressionElimination::SharedState::compute_barriers(
    cfg::ControlFlowGraph& cfg) {
  std::unordered_set<Barrier, BarrierHasher> barriers;
  for (auto& mie : cfg::InstructionIterable(cfg)) {
    auto* insn = mie.insn;
    if (may_be_barrier(insn)) {
      barriers.insert(make_barrier(insn));
    }
  }
  std::vector<Barrier> result(barriers.begin(), barriers.end());
  return result;
}

bool CommonSubexpressionElimination::SharedState::may_be_barrier(
    const IRInstruction* insn) {
  switch (insn->opcode()) {
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
    return true;
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
    return !is_invoke_safe(insn);
  default:
    if (insn->has_field()) {
      auto field_ref = insn->get_field();
      DexField* field = resolve_field(field_ref, is_sfield_op(insn->opcode())
                                                     ? FieldSearch::Static
                                                     : FieldSearch::Instance);
      return field == nullptr || is_volatile(field);
    }
    return false;
  }
}

bool CommonSubexpressionElimination::SharedState::is_barrier(
    const IRInstruction* insn, const ReadAccesses& read_accesses) {
  if (!may_be_barrier(insn)) {
    return false;
  }

  if (is_invoke(insn->opcode())) {
    return is_invoke_a_barrier(insn, read_accesses);
  } else {
    auto barrier = make_barrier(insn);
    return is_barrier_relevant(barrier, read_accesses);
  }
}

bool CommonSubexpressionElimination::SharedState::is_invoke_safe(
    const IRInstruction* insn) {
  always_assert(is_invoke(insn->opcode()));
  auto method_ref = insn->get_method();
  auto opcode = insn->opcode();

  if (opcode == OPCODE_INVOKE_STATIC &&
      m_safe_types.count(method_ref->get_class())) {
    return true;
  }

  if ((opcode == OPCODE_INVOKE_STATIC || opcode == OPCODE_INVOKE_DIRECT ||
       opcode == OPCODE_INVOKE_VIRTUAL) &&
      m_safe_methods.count(method_ref)) {
    return true;
  }

  return false;
}

bool CommonSubexpressionElimination::SharedState::is_invoke_a_barrier(
    const IRInstruction* insn, const ReadAccesses& read_accesses) {
  always_assert(is_invoke(insn->opcode()));

  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_STATIC || opcode == OPCODE_INVOKE_DIRECT ||
      opcode == OPCODE_INVOKE_VIRTUAL) {
    auto method_ref = insn->get_method();
    DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
    if (method != nullptr && (opcode != OPCODE_INVOKE_VIRTUAL ||
                              m_non_true_virtuals.count(method))) {
      auto it = m_method_barriers.find(method);
      if (it != m_method_barriers.end()) {
        auto& barriers = it->second;
        for (auto& barrier : barriers) {
          if (is_invoke(barrier.opcode) ||
              is_barrier_relevant(barrier, read_accesses)) {
            return true;
          }
        }
        return false;
      }
    }
  }

  return true;
}

bool CommonSubexpressionElimination::SharedState::is_barrier_relevant(
    const Barrier& barrier, const ReadAccesses& read_accesses) {
  auto op = barrier.opcode;
  switch (op) {
  case OPCODE_APUT:
    return read_accesses.array_component_types[INT];
  case OPCODE_APUT_BYTE:
    return read_accesses.array_component_types[BYTE];
  case OPCODE_APUT_CHAR:
    return read_accesses.array_component_types[CHAR];
  case OPCODE_APUT_WIDE:
    return read_accesses.array_component_types[WIDE];
  case OPCODE_APUT_SHORT:
    return read_accesses.array_component_types[SHORT];
  case OPCODE_APUT_OBJECT:
    return read_accesses.array_component_types[OBJECT];
  case OPCODE_APUT_BOOLEAN:
    return read_accesses.array_component_types[BOOLEAN];
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_FILL_ARRAY_DATA:
    return true;
  default:
    always_assert(is_sfield_op(op) || is_ifield_op(op));
    auto field = barrier.field;
    return field == nullptr || is_volatile(field) ||
           read_accesses.fields.count(nullptr) ||
           read_accesses.fields.count(field);
  }
}

void CommonSubexpressionElimination::SharedState::log_barrier(
    const Barrier& barrier) {
  if (m_barriers) {
    m_barriers->update(
        barrier, [](const Barrier, size_t& v, bool /* exists */) { v++; });
  }
}

void CommonSubexpressionElimination::SharedState::cleanup() {
  if (!m_barriers) {
    return;
  }

  std::vector<std::pair<Barrier, size_t>> ordered_barriers(m_barriers->begin(),
                                                           m_barriers->end());
  std::sort(
      ordered_barriers.begin(), ordered_barriers.end(),
      [](const std::pair<Barrier, size_t>& a,
         const std::pair<Barrier, size_t>& b) { return b.second < a.second; });

  TRACE(CSE, 2, "most common barriers:\n");
  for (auto& p : ordered_barriers) {
    auto& b = p.first;
    auto c = p.second;
    if (is_invoke(b.opcode)) {
      TRACE(CSE, 2, "%s %s x %u\n", SHOW(b.opcode), SHOW(b.method), c);
    } else if (is_ifield_op(b.opcode) || is_sfield_op(b.opcode)) {
      TRACE(CSE, 2, "%s %s x %u\n", SHOW(b.opcode), SHOW(b.field), c);
    } else {
      TRACE(CSE, 2, "%s x %u\n", SHOW(b.opcode), c);
    }
  }
}

CommonSubexpressionElimination::CommonSubexpressionElimination(
    SharedState* shared_state, cfg::ControlFlowGraph& cfg)
    : m_shared_state(shared_state), m_cfg(cfg) {
  Analyzer analyzer(shared_state, cfg);

  // identify all instruction pairs where the result of the first instruction
  // can be forwarded to the second

  for (cfg::Block* block : cfg.blocks()) {
    auto env = analyzer.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      analyzer.analyze_instruction(insn, &env);
      auto opcode = insn->opcode();
      if (!insn->dests_size() || is_move(opcode) || is_const(opcode)) {
        continue;
      }
      auto ref_c = env.get_ref_env().get(insn->dest()).get_constant();
      if (!ref_c) {
        continue;
      }
      auto value_id = *ref_c;
      always_assert(!analyzer.is_pre_state_src(value_id));
      auto ibs = analyzer.is_barrier_sensitive(value_id);
      auto def_c = env.get_def_env(ibs).get(value_id).get_constant();
      if (!def_c) {
        continue;
      }
      IRInstruction* earlier_insn = *def_c;
      if (earlier_insn == insn) {
        continue;
      }
      auto earlier_opcode = earlier_insn->opcode();
      if (opcode::is_load_param(earlier_opcode)) {
        continue;
      }

      m_forward.emplace_back((Forward){earlier_insn, insn});
    }
  }
}

bool CommonSubexpressionElimination::patch(bool is_static,
                                           DexType* declaring_type,
                                           DexTypeList* args) {
  if (m_forward.size() == 0) {
    return false;
  }

  TRACE(CSE, 5, "[CSE] before:\n%s\n", SHOW(m_cfg));

  // gather relevant instructions, and allocate temp registers

  std::unordered_map<IRInstruction*, std::pair<IROpcode, uint32_t>> temps;
  std::unordered_set<IRInstruction*> insns;
  for (auto& f : m_forward) {
    IRInstruction* earlier_insn = f.earlier_insn;
    if (!temps.count(earlier_insn)) {
      auto src_reg = earlier_insn->dest();
      uint32_t temp_reg;
      IROpcode move_opcode;
      if (earlier_insn->dest_is_object()) {
        move_opcode = OPCODE_MOVE_OBJECT;
        temp_reg = m_cfg.allocate_temp();
      } else if (earlier_insn->dest_is_wide()) {
        move_opcode = OPCODE_MOVE_WIDE;
        temp_reg = m_cfg.allocate_wide_temp();
      } else {
        move_opcode = OPCODE_MOVE;
        temp_reg = m_cfg.allocate_temp();
      }
      temps.emplace(earlier_insn, std::make_pair(move_opcode, temp_reg));
      insns.insert(earlier_insn);
    }

    insns.insert(f.insn);
  }

  // find all iterators in one sweep

  std::unordered_map<IRInstruction*, cfg::InstructionIterator> iterators;
  auto iterable = cfg::InstructionIterable(m_cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    auto* insn = it->insn;
    if (insns.count(insn)) {
      iterators.emplace(insn, it);
    }
  }

  // insert moves to use the forwarded value

  for (auto& f : m_forward) {
    IRInstruction* earlier_insn = f.earlier_insn;
    auto& q = temps.at(earlier_insn);
    IROpcode move_opcode = q.first;
    uint32_t temp_reg = q.second;
    IRInstruction* insn = f.insn;
    auto& it = iterators.at(insn);
    IRInstruction* move_insn = new IRInstruction(move_opcode);
    move_insn->set_src(0, temp_reg)->set_dest(insn->dest());
    m_cfg.insert_after(it, move_insn);

    TRACE(CSE, 4, "[CSE] forwarding %s to %s via v%u\n", SHOW(earlier_insn),
          SHOW(insn), temp_reg);
  }

  // insert moves to define the forwarded value

  for (auto& r : temps) {
    IRInstruction* earlier_insn = r.first;
    IROpcode move_opcode = r.second.first;
    uint32_t temp_reg = r.second.second;

    auto& it = iterators.at(earlier_insn);
    IRInstruction* move_insn = new IRInstruction(move_opcode);
    auto src_reg = earlier_insn->dest();
    move_insn->set_src(0, src_reg)->set_dest(temp_reg);
    m_cfg.insert_after(it, move_insn);
  }

  TRACE(CSE, 5, "[CSE] after:\n%s\n", SHOW(m_cfg));

  m_stats.instructions_eliminated += m_forward.size();
  m_stats.results_captured += temps.size();
  return true;
}

void CommonSubexpressionEliminationPass::run_pass(DexStoresVector& stores,
                                                  ConfigFiles& /* conf */,
                                                  PassManager& mgr) {
  const auto scope = build_class_scope(stores);

  auto shared_state = CommonSubexpressionElimination::SharedState();
  shared_state.init_method_barriers(scope);
  const auto stats =
      walk::parallel::reduce_methods<CommonSubexpressionElimination::Stats>(
          scope,
          [&](DexMethod* method) {
            const auto code = method->get_code();
            if (code == nullptr) {
              return CommonSubexpressionElimination::Stats();
            }

            TRACE(CSE, 3, "[CSE] processing %s\n", SHOW(method));
            CommonSubexpressionElimination cse(&shared_state, code->cfg());
            bool any_changes = cse.patch(is_static(method), method->get_class(),
                                         method->get_proto()->get_args());
            code->clear_cfg();
            if (any_changes) {
              // TODO: CopyPropagation and LocalDce will separately construct
              // an editable cfg. Don't do that, and fully convert those passes
              // to be cfg-based.

              CopyPropagationPass::Config config;
              copy_propagation_impl::CopyPropagation copy_propagation(config);
              copy_propagation.run(code, method);

              std::unordered_set<DexMethodRef*> pure_methods;
              auto local_dce = LocalDce(pure_methods);
              local_dce.dce(code);

              if (traceEnabled(CSE, 5)) {
                code->build_cfg(/* editable */ true);
                TRACE(CSE, 5, "[CSE] final:\n%s\n", SHOW(code->cfg()));
                code->clear_cfg();
              }
            }
            return cse.get_stats();
          },
          [](CommonSubexpressionElimination::Stats a,
             CommonSubexpressionElimination::Stats b) {
            a.results_captured += b.results_captured;
            a.instructions_eliminated += b.instructions_eliminated;
            return a;
          });
  mgr.incr_metric(METRIC_RESULTS_CAPTURED, stats.results_captured);
  mgr.incr_metric(METRIC_ELIMINATED_INSTRUCTIONS,
                  stats.instructions_eliminated);

  shared_state.cleanup();
}

static CommonSubexpressionEliminationPass s_pass;
