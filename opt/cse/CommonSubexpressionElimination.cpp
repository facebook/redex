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
#include "Purity.h"
#include "ReducedProductAbstractDomain.h"
#include "Resolver.h"
#include "TypeInference.h"
#include "Walkers.h"

using namespace sparta;
using namespace cse_impl;

namespace {

constexpr const char* METRIC_RESULTS_CAPTURED = "num_results_captured";
constexpr const char* METRIC_STORES_CAPTURED = "num_stores_captured";
constexpr const char* METRIC_ARRAY_LENGTHS_CAPTURED =
    "num_array_lengths_captured";
constexpr const char* METRIC_ELIMINATED_INSTRUCTIONS =
    "num_eliminated_instructions";
constexpr const char* METRIC_INLINED_BARRIERS_INTO_METHODS =
    "num_inlined_barriers_into_methods";
constexpr const char* METRIC_INLINED_BARRIERS_ITERATIONS =
    "num_inlined_barriers_iterations";
constexpr const char* METRIC_MAX_VALUE_IDS = "max_value_ids";
constexpr const char* METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT =
    "methods_using_other_tracked_location_bit";
constexpr const char* METRIC_INSTR_PREFIX = "instr_";

using value_id_t = uint64_t;
enum ValueIdFlags : value_id_t {
  // lower bits for tracked locations
  IS_NOT_READ_ONLY_WRITTEN_LOCATION = 0,
  IS_FIRST_TRACKED_LOCATION = ((value_id_t)1),
  IS_OTHER_TRACKED_LOCATION = ((value_id_t)1) << (sizeof(value_id_t) * 4),
  IS_ONLY_READ_NOT_WRITTEN_LOCATION = ((value_id_t)1)
                                      << (sizeof(value_id_t) * 4 + 1),
  IS_TRACKED_LOCATION_MASK = IS_ONLY_READ_NOT_WRITTEN_LOCATION * 2 - 1,
  IS_PRE_STATE_SRC = ((value_id_t)1) << (sizeof(value_id_t) * 4 + 2),
  // upper bits for unique values
  BASE = ((value_id_t)1) << (sizeof(value_id_t) * 4 + 3),
};

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

// Marker opcode for values representing a source of an instruction; this is
// used to recover from merged / havoced values.
const IROpcode IOPCODE_PRE_STATE_SRC = IROpcode(0xFFFF);

// Marker opcode for positional values that must not be moved.
const IROpcode IOPCODE_POSITIONAL = IROpcode(0xFFFE);

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

static Barrier make_barrier(const IRInstruction* insn) {
  Barrier b;
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

Location get_field_location(IROpcode opcode, const DexField* field) {
  always_assert(is_ifield_op(opcode) || is_sfield_op(opcode));
  if (field != nullptr && !is_volatile(field)) {
    return Location(field);
  }

  return Location(GENERAL_MEMORY_BARRIER);
}

Location get_field_location(IROpcode opcode, const DexFieldRef* field_ref) {
  always_assert(is_ifield_op(opcode) || is_sfield_op(opcode));
  DexField* field =
      resolve_field(field_ref, is_sfield_op(opcode) ? FieldSearch::Static
                                                    : FieldSearch::Instance);
  return get_field_location(opcode, field);
}

Location get_written_array_location(IROpcode opcode) {
  switch (opcode) {
  case OPCODE_APUT:
    return Location(ARRAY_COMPONENT_TYPE_INT);
  case OPCODE_APUT_BYTE:
    return Location(ARRAY_COMPONENT_TYPE_BYTE);
  case OPCODE_APUT_CHAR:
    return Location(ARRAY_COMPONENT_TYPE_CHAR);
  case OPCODE_APUT_WIDE:
    return Location(ARRAY_COMPONENT_TYPE_WIDE);
  case OPCODE_APUT_SHORT:
    return Location(ARRAY_COMPONENT_TYPE_SHORT);
  case OPCODE_APUT_OBJECT:
    return Location(ARRAY_COMPONENT_TYPE_OBJECT);
  case OPCODE_APUT_BOOLEAN:
    return Location(ARRAY_COMPONENT_TYPE_BOOLEAN);
  default:
    always_assert(false);
  }
}

Location get_written_location(const Barrier& barrier) {
  if (is_aput(barrier.opcode)) {
    return get_written_array_location(barrier.opcode);
  } else if (is_iput(barrier.opcode) || is_sput(barrier.opcode)) {
    return get_field_location(barrier.opcode, barrier.field);
  } else {
    return Location(GENERAL_MEMORY_BARRIER);
  }
}

Location get_read_array_location(IROpcode opcode) {
  switch (opcode) {
  case OPCODE_AGET:
    return Location(ARRAY_COMPONENT_TYPE_INT);
  case OPCODE_AGET_BYTE:
    return Location(ARRAY_COMPONENT_TYPE_BYTE);
  case OPCODE_AGET_CHAR:
    return Location(ARRAY_COMPONENT_TYPE_CHAR);
  case OPCODE_AGET_WIDE:
    return Location(ARRAY_COMPONENT_TYPE_WIDE);
  case OPCODE_AGET_SHORT:
    return Location(ARRAY_COMPONENT_TYPE_SHORT);
  case OPCODE_AGET_OBJECT:
    return Location(ARRAY_COMPONENT_TYPE_OBJECT);
  case OPCODE_AGET_BOOLEAN:
    return Location(ARRAY_COMPONENT_TYPE_BOOLEAN);
  default:
    always_assert(false);
  }
}

Location get_read_location(const IRInstruction* insn) {
  if (is_aget(insn->opcode())) {
    return get_read_array_location(insn->opcode());
  } else if (is_iget(insn->opcode()) || is_sget(insn->opcode())) {
    return get_field_location(insn->opcode(), insn->get_field());
  } else {
    return Location(GENERAL_MEMORY_BARRIER);
  }
}

bool is_barrier_relevant(
    const Barrier& barrier,
    const std::unordered_set<Location, LocationHasher>& read_locations) {
  auto location = get_written_location(barrier);
  return location == Location(GENERAL_MEMORY_BARRIER) ||
         read_locations.count(location);
}

template <class Set>
bool are_disjoint(const Set* s, const Set* t) {
  if (s->size() > t->size()) {
    std::swap(s, t);
  }
  for (const auto& elem : *s) {
    if (t->count(elem)) {
      return false;
    }
  }
  return true;
}

class Analyzer final : public BaseIRAnalyzer<CseEnvironment> {
 public:
  Analyzer(SharedState* shared_state, cfg::ControlFlowGraph& cfg)
      : BaseIRAnalyzer(cfg), m_shared_state(shared_state) {
    std::unordered_map<Location, size_t, LocationHasher> read_location_counts;
    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto location = get_read_location(mie.insn);
      if (location != Location(GENERAL_MEMORY_BARRIER)) {
        read_location_counts[location]++;
        m_read_locations.insert(location);
      }
    }

    std::unordered_map<Location, size_t, LocationHasher>
        written_location_counts;
    for (auto& mie : cfg::InstructionIterable(cfg)) {
      auto location = shared_state->get_relevant_written_location(
          mie.insn, nullptr /* exact_virtual_scope */, m_read_locations);
      if (location) {
        written_location_counts[*location]++;
      }
    }

    std::vector<Location> read_and_written_locations;
    for (auto& p : written_location_counts) {
      if (read_location_counts.count(p.first)) {
        read_and_written_locations.push_back(p.first);
      } else if (p.first != Location(GENERAL_MEMORY_BARRIER)) {
        m_tracked_locations.emplace(
            p.first, ValueIdFlags::IS_NOT_READ_ONLY_WRITTEN_LOCATION);
      }
    }
    for (auto& p : read_location_counts) {
      if (!written_location_counts.count(p.first)) {
        m_tracked_locations.emplace(
            p.first, ValueIdFlags::IS_ONLY_READ_NOT_WRITTEN_LOCATION);
      }
    }
    // We'll use roughly half of the bits in a value_id to encode what kind of
    // heap locations were involved in producing the value, so that we can
    // later quickly identify which values need to be invalidated when
    // encountering a write to a specific location. However, we only have a
    // limited number of bits available, and potentially many more relevant
    // locations.
    //
    // We'll identify the long tail of locations that are read and written via a
    // separate bit (IS_OTHER_TRACKED_LOCATION), and we'll also reserve one bit
    // for locations that are read but not written
    // (IS_ONLY_READ_NOT_WRITTEN_LOCATION), so that we can identify these
    // heap-dependent locations when we need to validate all heap-dependent
    // locations in case of a general memory barrier.
    //
    // We use a heuristic to decide which locations get their own bit, vs
    // the long-tail treatment.
    //
    // The currently implemented heuristic prefers locations that are often
    // read and rarely written.
    //
    // TODO: Explore other (variations of this) heuristics.

    std::sort(read_and_written_locations.begin(),
              read_and_written_locations.end(), [&](Location a, Location b) {
                auto get_weight = [&](Location l) {
                  auto reads = read_location_counts.at(l);
                  auto writes = written_location_counts.at(l);
                  return (reads << 16) / writes;
                };
                auto weight_a = get_weight(a);
                auto weight_b = get_weight(b);
                if (weight_a != weight_b) {
                  // higher weight takes precedence
                  return weight_a > weight_b;
                }
                // in case of a tie, still ensure a deterministic total ordering
                return a < b;
              });
    TRACE(CSE, 4, "[CSE] relevant locations: %u %s",
          read_and_written_locations.size(),
          read_and_written_locations.size() > 13 ? "(HUGE!)" : "");
    value_id_t next_bit = ValueIdFlags::IS_FIRST_TRACKED_LOCATION;
    for (auto l : read_and_written_locations) {
      TRACE(CSE, 4, "[CSE]   %s: %u reads, %u writes",
            l.special_location < END ? "array element" : SHOW(l.field),
            read_location_counts.at(l), written_location_counts.at(l));
      m_tracked_locations.emplace(l, next_bit);
      if (next_bit == ValueIdFlags::IS_OTHER_TRACKED_LOCATION) {
        m_using_other_tracked_location_bit = true;
      } else {
        // we've already reached the last catch-all tracked read/write location
        next_bit <<= 1;
      }
    }

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
    default: {
      // If we get here, reset destination.
      if (insn->has_dest()) {
        ValueIdDomain domain;
        if (opcode::is_move_result_any(opcode)) {
          domain = current_state->get_ref_env().get(RESULT_REGISTER);
        } else {
          domain = get_value_id_domain(insn, current_state);
        }
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
      } else if (insn->has_move_result_any()) {
        ValueIdDomain domain = get_value_id_domain(insn, current_state);
        current_state->mutate_ref_env(
            [&](RefEnvironment* env) { env->set(RESULT_REGISTER, domain); });
        if (opcode == OPCODE_NEW_ARRAY && domain.get_constant()) {
          auto value = get_array_length_value(*domain.get_constant());
          TRACE(CSE, 4, "[CSE] installing array-length forwarding for %s",
                SHOW(insn));
          install_forwarding(insn, value, current_state);
        }
      }
      break;
    }
    }

    auto location = get_clobbered_location(insn, current_state);
    if (location) {
      auto mask = get_location_value_id_mask(*location);

      // TODO: The following loops are probably the most expensive thing in this
      // algorithm; is there a better way of doing this? (Then again, overall,
      // the time this algorithm takes seems reasonable.)

      bool any_changes = false;
      current_state->mutate_def_env(true /* is_barrier_sensitive */,
                                    [mask, &any_changes](DefEnvironment* env) {
                                      if (env->erase_all_matching(mask)) {
                                        any_changes = true;
                                      }
                                    });
      current_state->mutate_ref_env([mask, &any_changes](RefEnvironment* env) {
        bool any_map_changes = env->map([mask](ValueIdDomain domain) {
          auto c = domain.get_constant();
          always_assert(c);
          auto value_id = *c;
          if (value_id & mask) {
            return ValueIdDomain::top();
          }
          return domain;
        });
        if (any_map_changes) {
          any_changes = true;
        }
      });
      if (any_changes) {
        m_shared_state->log_barrier(make_barrier(insn));
      }

      if (location != Location(GENERAL_MEMORY_BARRIER)) {
        auto value = get_equivalent_put_value(insn, current_state);
        if (value) {
          TRACE(CSE, 4, "[CSE] installing store-to-load forwarding for %s",
                SHOW(insn));
          install_forwarding(insn, *value, current_state);
        }
      }
    }
  }

  void install_forwarding(IRInstruction* insn,
                          const IRValue& value,
                          CseEnvironment* current_state) const {
    auto value_id = *get_value_id(value);
    auto ibs = is_barrier_sensitive(value_id);
    auto insn_domain = IRInstructionDomain(insn);
    current_state->mutate_def_env(ibs,
                                  [value_id, insn_domain](DefEnvironment* env) {
                                    env->set(value_id, insn_domain);
                                  });
  }

  bool is_pre_state_src(value_id_t value_id) const {
    return !!(value_id & ValueIdFlags::IS_PRE_STATE_SRC);
  }

  bool is_barrier_sensitive(value_id_t value_id) const {
    return !!(value_id & ValueIdFlags::IS_TRACKED_LOCATION_MASK);
  }

  size_t get_value_ids_size() { return m_value_ids.size(); }

  bool using_other_tracked_location_bit() {
    return m_using_other_tracked_location_bit;
  }

 private:
  boost::optional<Location> get_clobbered_location(
      const IRInstruction* insn, CseEnvironment* current_state) const {
    DexType* exact_virtual_scope = nullptr;
    if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
      auto src0 = current_state->get_ref_env().get(insn->src(0)).get_constant();
      if (src0) {
        exact_virtual_scope = get_exact_type(*src0);
      }
    }
    return m_shared_state->get_relevant_written_location(
        insn, exact_virtual_scope, m_read_locations);
  }

  ValueIdDomain get_value_id_domain(const IRInstruction* insn,
                                    CseEnvironment* current_state) const {
    auto value = get_value(insn, current_state);
    auto value_id = get_value_id(value);
    return value_id ? ValueIdDomain(*value_id) : ValueIdDomain::top();
  }

  value_id_t get_pre_state_src_value_id(register_t reg,
                                        const IRInstruction* insn) const {
    auto value = get_pre_state_src_value(reg, insn);
    auto value_id = get_value_id(value);
    always_assert(value_id);
    return *value_id;
  }

  boost::optional<value_id_t> get_value_id(const IRValue& value) const {
    auto it = m_value_ids.find(value);
    if (it != m_value_ids.end()) {
      return boost::optional<value_id_t>(it->second);
    }
    value_id_t id = m_value_ids.size() * ValueIdFlags::BASE;
    always_assert(id / ValueIdFlags::BASE == m_value_ids.size());
    if (is_aget(value.opcode)) {
      id |= get_location_value_id_mask(get_read_array_location(value.opcode));
    } else if (is_iget(value.opcode) || is_sget(value.opcode)) {
      auto location = get_field_location(value.opcode, value.field);
      if (location == Location(GENERAL_MEMORY_BARRIER)) {
        return boost::none;
      }
      id |= get_location_value_id_mask(location);
    } else if (value.opcode == IOPCODE_PRE_STATE_SRC) {
      id |= ValueIdFlags::IS_PRE_STATE_SRC;
    }
    if (value.opcode != IOPCODE_PRE_STATE_SRC) {
      for (auto src : value.srcs) {
        id |= (src & ValueIdFlags::IS_TRACKED_LOCATION_MASK);
      }
    }
    m_value_ids.emplace(value, id);
    if (value.opcode == IOPCODE_POSITIONAL) {
      m_positional_insns.emplace(id, value.positional_insn);
    }
    return boost::optional<value_id_t>(id);
  }

  IRValue get_array_length_value(value_id_t array_value_id) const {
    IRValue value;
    value.opcode = OPCODE_ARRAY_LENGTH;
    value.srcs.push_back(array_value_id);
    return value;
  }

  boost::optional<IRValue> get_equivalent_put_value(
      IRInstruction* insn, CseEnvironment* current_state) const {
    auto ref_env = current_state->get_ref_env();
    if (is_sput(insn->opcode())) {
      always_assert(insn->srcs_size() == 1);
      IRValue value;
      value.opcode = (IROpcode)(insn->opcode() - OPCODE_SPUT + OPCODE_SGET);
      value.field = insn->get_field();
      return value;
    } else if (is_iput(insn->opcode())) {
      always_assert(insn->srcs_size() == 2);
      auto src1 = ref_env.get(insn->src(1)).get_constant();
      if (src1) {
        IRValue value;
        value.opcode = (IROpcode)(insn->opcode() - OPCODE_IPUT + OPCODE_IGET);
        value.srcs.push_back(*src1);
        value.field = insn->get_field();
        return value;
      }
    } else if (is_aput(insn->opcode())) {
      always_assert(insn->srcs_size() == 3);
      auto src1 = ref_env.get(insn->src(1)).get_constant();
      auto src2 = ref_env.get(insn->src(2)).get_constant();
      if (src1 && src2) {
        IRValue value;
        value.opcode = (IROpcode)(insn->opcode() - OPCODE_APUT + OPCODE_AGET);
        value.srcs.push_back(*src1);
        value.srcs.push_back(*src2);
        return value;
      }
    }
    return boost::none;
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
    case OPCODE_INVOKE_INTERFACE: {
      // TODO: Is this really safe for all virtual/interface invokes? This
      //       mimics the way assumenosideeffects is used in LocalDCE.
      is_positional = !m_shared_state->has_pure_method(insn);
      break;
    }
    default:
      auto location = m_shared_state->get_relevant_written_location(
          insn, nullptr /* exact_virtual_scope */, m_read_locations);
      is_positional = !!location;
      break;
    }
    if (is_positional) {
      value.opcode = IOPCODE_POSITIONAL;
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

  value_id_t get_location_value_id_mask(Location l) const {
    if (l == Location(GENERAL_MEMORY_BARRIER)) {
      return ValueIdFlags::IS_TRACKED_LOCATION_MASK;
    } else {
      return m_tracked_locations.at(l);
    }
  }

  DexType* get_exact_type(value_id_t value_id) const {
    auto it = m_positional_insns.find(value_id);
    if (it == m_positional_insns.end()) {
      return nullptr;
    }
    auto insn = it->second;
    switch (insn->opcode()) {
    case OPCODE_NEW_ARRAY:
    case OPCODE_NEW_INSTANCE:
    case OPCODE_FILLED_NEW_ARRAY:
      return insn->get_type();
    default:
      return nullptr;
    }
  }

  bool m_using_other_tracked_location_bit{false};
  std::unordered_set<Location, LocationHasher> m_read_locations;
  std::unordered_map<Location, value_id_t, LocationHasher> m_tracked_locations;
  SharedState* m_shared_state;
  mutable std::unordered_map<IRValue, value_id_t, IRValueHasher> m_value_ids;
  mutable std::unordered_map<value_id_t, const IRInstruction*>
      m_positional_insns;
};

} // namespace

namespace cse_impl {

////////////////////////////////////////////////////////////////////////////////

SharedState::SharedState(const std::unordered_set<DexMethodRef*>& pure_methods)
    : m_pure_methods(pure_methods), m_safe_methods(pure_methods) {
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
  static const char* safe_method_names[] = {
      "Landroid/os/SystemClock;.elapsedRealtime:()J",
      "Landroid/os/SystemClock;.uptimeMillis:()J",
      "Landroid/util/SparseArray;.append:(ILjava/lang/Object;)V",
      "Landroid/util/SparseArray;.get:(I)Ljava/lang/Object;",
      "Landroid/util/SparseArray;.put:(ILjava/lang/Object;)V",
      "Landroid/util/SparseArray;.size:()I",
      "Landroid/util/SparseArray;.valueAt:(I)Ljava/lang/Object;",
      "Landroid/util/SparseIntArray;.put:(II)V",
      "Ljava/lang/Boolean;.parseBoolean:(Ljava/lang/String;)Z",
      "Ljava/lang/Byte;.parseByte:(Ljava/lang/String;)B",
      "Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class;",
      "Ljava/lang/Double;.parseDouble:(Ljava/lang/String;)D",
      "Ljava/lang/Enum;.valueOf:(Ljava/lang/Class;Ljava/lang/String;)Ljava/"
      "lang/Enum;",
      "Ljava/lang/Float;.parseFloat:(Ljava/lang/String;)F",
      "Ljava/lang/Integer;.parseInt:(Ljava/lang/String;)I",
      "Ljava/lang/Integer;.parseInt:(Ljava/lang/String;I)I",
      "Ljava/lang/Integer;.valueOf:(Ljava/lang/String;)Ljava/lang/Integer;",
      "Ljava/lang/Long;.parseLong:(Ljava/lang/String;)J",
      "Ljava/lang/Math;.addExact:(II)I",
      "Ljava/lang/Math;.addExact:(JJ)J",
      "Ljava/lang/Math;.decrementExact:(J)J",
      "Ljava/lang/Math;.decrementExact:(I)I",
      "Ljava/lang/Math;.incrementExact:(I)I",
      "Ljava/lang/Math;.incrementExact:(J)J",
      "Ljava/lang/Math;.multiplyExact:(II)I",
      "Ljava/lang/Math;.multiplyExact:(JJ)J",
      "Ljava/lang/Math;.negateExact:(I)I",
      "Ljava/lang/Math;.negateExact:(J)J",
      "Ljava/lang/Math;.subtractExact:(JJ)J",
      "Ljava/lang/Math;.subtractExact:(II)I",
      "Ljava/lang/Math;.toIntExact:(J)I",
      "Ljava/lang/ref/Reference;.get:()Ljava/lang/Object;",
      "Ljava/lang/String;.getBytes:()[B",
      "Ljava/lang/String;.split:(Ljava/lang/String;)[Ljava/lang/String;",
      "Ljava/lang/StringBuilder;.append:(C)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(I)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(J)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
      "StringBuilder;",
      "Ljava/lang/StringBuilder;.append:(Z)Ljava/lang/StringBuilder;",
      "Ljava/lang/StringBuilder;.length:()I",
      "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;",
      "Ljava/lang/System;.currentTimeMillis:()J",
      "Ljava/lang/System;.nanoTime:()J",
      "Ljava/util/ArrayList;.add:(Ljava/lang/Object;)Z",
      "Ljava/util/ArrayList;.add:(ILjava/lang/Object;)V",
      "Ljava/util/ArrayList;.clear:()V",
      "Ljava/util/ArrayList;.get:(I)Ljava/lang/Object;",
      "Ljava/util/ArrayList;.isEmpty:()Z",
      "Ljava/util/ArrayList;.remove:(I)Ljava/lang/Object;",
      "Ljava/util/ArrayList;.size:()I",
      "Ljava/util/BitSet;.clear:()V",
      "Ljava/util/BitSet;.get:(I)Z",
      "Ljava/util/BitSet;.set:(I)V",
      "Ljava/util/HashMap;.isEmpty:()Z",
      "Ljava/util/HashMap;.size:()I",
      "Ljava/util/HashSet;.clear:()V",
      "Ljava/util/LinkedList;.add:(Ljava/lang/Object;)Z",
      "Ljava/util/LinkedList;.addLast:(Ljava/lang/Object;)V",
      "Ljava/util/LinkedList;.clear:()V",
      "Ljava/util/LinkedList;.get:(I)Ljava/lang/Object;",
      "Ljava/util/LinkedList;.getFirst:()Ljava/lang/Object;",
      "Ljava/util/LinkedList;.removeFirst:()Ljava/lang/Object;",
      "Ljava/util/LinkedList;.size:()I",
      "Ljava/util/Random;.nextInt:(I)I",

      "Landroid/util/Pair;.<init>:(Ljava/lang/Object;Ljava/lang/Object;)V",
      "Landroid/util/SparseArray;.<init>:()V",
      "Ljava/io/IOException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V",
      "Ljava/lang/Exception;.<init>:()V",
      "Ljava/lang/IllegalArgumentException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/IllegalStateException;.<init>:()V",
      "Ljava/lang/IllegalStateException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Integer;.<init>:(I)V",
      "Ljava/lang/Long;.<init>:(J)V",
      "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Object;.<init>:()V",
      "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/Short;.<init>:(S)V",
      "Ljava/lang/String;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/StringBuilder;.<init>:()V",
      "Ljava/lang/StringBuilder;.<init>:(I)V",
      "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V",
      "Ljava/lang/UnsupportedOperationException;.<init>:(Ljava/lang/"
      "String;)V",
      "Ljava/util/ArrayList;.<init>:()V",
      "Ljava/util/ArrayList;.<init>:(I)V",
      "Ljava/util/BitSet;.<init>:(I)V",
      "Ljava/util/HashMap;.<init>:()V",
      "Ljava/util/HashMap;.<init>:(I)V",
      "Ljava/util/HashSet;.<init>:()V",
      "Ljava/util/LinkedHashMap;.<init>:()V",
      "Ljava/util/LinkedList;.<init>:()V",
      "Ljava/util/Random;.<init>:()V",
  };

  for (auto const safe_method_name : safe_method_names) {
    const std::string& s(safe_method_name);
    auto method_ref = DexMethod::get_method(s);
    if (method_ref == nullptr) {
      TRACE(CSE, 1, "[CSE]: Could not find safe method %s", s.c_str());
      continue;
    }

    m_safe_methods.insert(method_ref);
  }

  // Check that we don't have abstract or interface methods
  for (DexMethodRef* method_ref : m_safe_methods) {
    if (method_ref->is_def()) {
      always_assert(!is_interface(type_class(method_ref->get_class())));
      auto method = static_cast<DexMethod*>(method_ref);
      always_assert(!is_abstract(method));
    }
  }

  if (traceEnabled(CSE, 2)) {
    m_barriers.reset(new ConcurrentMap<Barrier, size_t, BarrierHasher>());
  }
}

MethodBarriersStats SharedState::init_method_barriers(const Scope& scope,
                                                      size_t max_iterations) {
  m_method_override_graph = method_override_graph::build_graph(scope);

  ConcurrentMap<const DexMethod*, std::vector<Barrier>> method_barriers;
  ConcurrentMap<const DexMethod*, const DexMethod*> waiting_for;
  // Let's initialize method_barriers, and waiting_for.
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method->rstate.no_optimizations()) {
      waiting_for.emplace(method, nullptr);
      return;
    }
    code.build_cfg(/* editable */ true);
    std::unordered_set<Barrier, BarrierHasher> set;
    boost::optional<const DexMethod*> wait_for_method;
    for (auto& mie : cfg::InstructionIterable(code.cfg())) {
      auto* insn = mie.insn;
      if (may_be_barrier(insn, nullptr /* exact_virtual_scope */)) {
        auto barrier = make_barrier(insn);
        get_written_location(barrier);
        set.insert(barrier);
        if (is_invoke(barrier.opcode)) {
          wait_for_method = barrier.method;
        }
      }
    }
    std::vector<Barrier> barriers(set.begin(), set.end());
    method_barriers.emplace(method, barriers);
    if (wait_for_method) {
      waiting_for.emplace(method, *wait_for_method);
    }
  });

  // Let's try to (semantically) inline barriers: We merge sets of
  // barriers, looking into invocations. We do it incrementally,
  // each time only from methods that do not call any other methods.
  MethodBarriersStats stats;
  for (size_t iterations = 0; iterations < max_iterations; iterations++) {
    stats.inlined_barriers_iterations++;

    ConcurrentMap<const DexMethod*, std::vector<Barrier>>
        updated_method_barriers;
    ConcurrentMap<const DexMethod*, const DexMethod*> updated_waiting_for;
    walk::parallel::code(scope, [&](DexMethod* method, IRCode&) {
      auto waiting_for_it = waiting_for.find(method);
      if (waiting_for_it == waiting_for.end()) {
        // no invocation to inline
        return;
      }

      auto can_inline_barriers = [&](const DexMethod* other_method) {
        if (other_method == nullptr) {
          return false;
        }
        if (is_abstract(other_method) || assumenosideeffects(other_method)) {
          return true;
        }
        // we'll only inline methods that themselves do not have any further
        // calls
        return !waiting_for.count_unsafe(other_method) &&
               !other_method->is_external() && !is_native(other_method);
      };

      // Quick check: Are we are waiting for a method that cannot be inlined
      // (yet)?
      auto waiting_for_method = waiting_for_it->second;
      if (!can_inline_barriers(waiting_for_method)) {
        return;
      }

      std::unordered_set<Barrier, BarrierHasher> barriers;
      auto inline_barriers = [&](const DexMethod* other_method) {
        if (!is_abstract(other_method) && !assumenosideeffects(other_method)) {
          always_assert(!waiting_for.count_unsafe(other_method));
          always_assert(!other_method->is_external());
          always_assert(!is_native(other_method));
          always_assert(other_method->get_code());
          always_assert(can_inline_barriers(other_method));
          always_assert(method_barriers.count_unsafe(other_method));
          auto& invoked_barriers = method_barriers.at_unsafe(other_method);
          std::copy(invoked_barriers.begin(), invoked_barriers.end(),
                    std::inserter(barriers, barriers.end()));
        }
      };

      for (auto& barrier : method_barriers.at_unsafe(method)) {
        if (!is_invoke(barrier.opcode)) {
          barriers.insert(barrier);
          continue;
        }

        if (barrier.opcode == OPCODE_INVOKE_SUPER) {
          // TODO: Implement
          updated_waiting_for.insert_or_assign(std::make_pair(method, nullptr));
          return;
        }

        if (!can_inline_barriers(barrier.method)) {
          // giving up, won't inline anything as it's pointless
          updated_waiting_for.insert_or_assign(
              std::make_pair(method, barrier.method));
          return;
        }

        inline_barriers(barrier.method);

        if (barrier.opcode == OPCODE_INVOKE_VIRTUAL ||
            barrier.opcode == OPCODE_INVOKE_INTERFACE) {
          always_assert(barrier.method->is_virtual());
          const auto overriding_methods =
              method_override_graph::get_overriding_methods(
                  *m_method_override_graph, barrier.method);
          for (const DexMethod* overriding_method : overriding_methods) {
            if (!can_inline_barriers(overriding_method)) {
              // giving up, won't inline anything as it's pointless
              updated_waiting_for.insert_or_assign(
                  std::make_pair(method, overriding_method));
              return;
            }

            inline_barriers(overriding_method);
          }
        }
      }

      std::vector<Barrier> vector(barriers.begin(), barriers.end());
      updated_method_barriers.emplace(method, vector);
    });

    if (updated_method_barriers.size() == 0) {
      break;
    }

    for (auto& p : updated_waiting_for) {
      waiting_for.insert_or_assign(p);
      always_assert(!updated_method_barriers.count(p.first));
    }

    for (auto& p : updated_method_barriers) {
      method_barriers.insert_or_assign(p);
      always_assert(!updated_waiting_for.count_unsafe(p.first));
      always_assert(waiting_for.count_unsafe(p.first));
      waiting_for.erase(p.first);
      stats.inlined_barriers_into_methods++;
    }
  }

  for (auto& p : method_barriers) {
    std::unordered_set<Location, LocationHasher>& written_locations =
        m_method_written_locations[p.first];
    for (auto& barrier : p.second) {
      written_locations.insert(get_written_location(barrier));
    }
  }

  return stats;
}

boost::optional<Location> SharedState::get_relevant_written_location(
    const IRInstruction* insn,
    DexType* exact_virtual_scope,
    const std::unordered_set<Location, LocationHasher>& read_locations) {
  if (may_be_barrier(insn, exact_virtual_scope)) {
    if (is_invoke(insn->opcode())) {
      if (is_invoke_a_barrier(insn, read_locations)) {
        return boost::optional<Location>(Location(GENERAL_MEMORY_BARRIER));
      }
    } else {
      auto barrier = make_barrier(insn);
      if (is_barrier_relevant(barrier, read_locations)) {
        return boost::optional<Location>(get_written_location(barrier));
      }
    }
  }
  return boost::none;
}

bool SharedState::may_be_barrier(const IRInstruction* insn,
                                 DexType* exact_virtual_scope) {
  auto opcode = insn->opcode();
  switch (opcode) {
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_FILL_ARRAY_DATA:
    return true;
  default:
    if (is_aput(opcode) || is_iput(opcode) || is_sput(opcode)) {
      return true;
    } else if (is_invoke(opcode)) {
      return !is_invoke_safe(insn, exact_virtual_scope);
    }
    if (insn->has_field()) {
      always_assert(is_iget(opcode) || is_sget(opcode));
      if (get_field_location(opcode, insn->get_field()) ==
          Location(GENERAL_MEMORY_BARRIER)) {
        return true;
      }
    }

    return false;
  }
}

bool SharedState::is_invoke_safe(const IRInstruction* insn,
                                 DexType* exact_virtual_scope) {
  always_assert(is_invoke(insn->opcode()));
  auto method_ref = insn->get_method();
  auto opcode = insn->opcode();

  if ((opcode == OPCODE_INVOKE_STATIC || opcode == OPCODE_INVOKE_DIRECT) &&
      m_safe_methods.count(method_ref)) {
    return true;
  }

  auto method = resolve_method(method_ref, opcode_to_search(insn));
  if (!method) {
    return false;
  }

  if ((opcode == OPCODE_INVOKE_STATIC || opcode == OPCODE_INVOKE_DIRECT) &&
      m_safe_methods.count(method)) {
    return true;
  }

  if (opcode == OPCODE_INVOKE_VIRTUAL && m_safe_methods.count(method)) {
    auto type = method->get_class();
    auto cls = type_class(type);
    always_assert(cls);
    if (is_final(cls) || is_final(method)) {
      return true;
    }
    if (type == exact_virtual_scope) {
      return true;
    }
  }

  return false;
}

bool SharedState::is_invoke_a_barrier(
    const IRInstruction* insn,
    const std::unordered_set<Location, LocationHasher>& read_locations) {
  always_assert(is_invoke(insn->opcode()));

  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_SUPER) {
    // TODO
    return true;
  }

  auto has_barriers = [&](const DexMethod* method) {
    if (method->is_external() || is_native(method)) {
      return true;
    }
    if (is_abstract(method)) {
      // We say abstract methods are not a barrier per se, as we'll inspect all
      // overriding methods further below.
      return false;
    }
    auto it = m_method_written_locations.find(method);
    if (it == m_method_written_locations.end()) {
      return true;
    }
    auto& written_locations = it->second;
    if (written_locations.count(Location(GENERAL_MEMORY_BARRIER))) {
      return true;
    }
    return !are_disjoint(&written_locations, &read_locations);
  };

  auto method_ref = insn->get_method();
  DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
  if (method == nullptr || has_barriers(method)) {
    return true;
  }
  if (opcode == OPCODE_INVOKE_VIRTUAL || opcode == OPCODE_INVOKE_INTERFACE) {
    always_assert(method->is_virtual());
    if (!m_method_override_graph) {
      return true;
    }
    const auto overriding_methods =
        method_override_graph::get_overriding_methods(*m_method_override_graph,
                                                      method);
    for (const DexMethod* overriding_method : overriding_methods) {
      if (has_barriers(overriding_method)) {
        return true;
      }
    }
  }
  return false;
}

void SharedState::log_barrier(const Barrier& barrier) {
  if (m_barriers) {
    m_barriers->update(
        barrier, [](const Barrier, size_t& v, bool /* exists */) { v++; });
  }
}

bool SharedState::has_pure_method(const IRInstruction* insn) const {
  auto method_ref = insn->get_method();
  if (m_pure_methods.find(method_ref) != m_pure_methods.end()) {
    TRACE(CSE, 4, "[CSE] unresolved pure for %s", SHOW(method_ref));
    return true;
  }

  auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (method != nullptr &&
      m_pure_methods.find(method) != m_pure_methods.end()) {
    TRACE(CSE, 4, "[CSE] resolved pure for %s", SHOW(method));
    return true;
  }

  return false;
}

void SharedState::cleanup() {
  if (!m_barriers) {
    return;
  }

  std::vector<std::pair<Barrier, size_t>> ordered_barriers(m_barriers->begin(),
                                                           m_barriers->end());
  std::sort(
      ordered_barriers.begin(), ordered_barriers.end(),
      [](const std::pair<Barrier, size_t>& a,
         const std::pair<Barrier, size_t>& b) { return b.second < a.second; });

  TRACE(CSE, 2, "most common barriers:");
  for (auto& p : ordered_barriers) {
    auto& b = p.first;
    auto c = p.second;
    if (is_invoke(b.opcode)) {
      TRACE(CSE, 2, "%s %s x %u", SHOW(b.opcode), SHOW(b.method), c);
    } else if (is_ifield_op(b.opcode) || is_sfield_op(b.opcode)) {
      TRACE(CSE, 2, "%s %s x %u", SHOW(b.opcode), SHOW(b.field), c);
    } else {
      TRACE(CSE, 2, "%s x %u", SHOW(b.opcode), c);
    }
  }
}

CommonSubexpressionElimination::CommonSubexpressionElimination(
    SharedState* shared_state, cfg::ControlFlowGraph& cfg)
    : m_shared_state(shared_state), m_cfg(cfg) {
  Analyzer analyzer(shared_state, cfg);
  m_stats.max_value_ids = analyzer.get_value_ids_size();
  if (analyzer.using_other_tracked_location_bit()) {
    m_stats.methods_using_other_tracked_location_bit = 1;
  }

  // identify all instruction pairs where the result of the first instruction
  // can be forwarded to the second

  for (cfg::Block* block : cfg.blocks()) {
    auto env = analyzer.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      analyzer.analyze_instruction(insn, &env);
      auto opcode = insn->opcode();
      if (!insn->has_dest() || is_move(opcode) || is_const(opcode)) {
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
      if (opcode::is_cmp(opcode) || opcode::is_cmp(earlier_opcode)) {
        // See T46241704. We never de-duplicate cmp instructions due to an
        // apparent bug in various Dalvik (and ART?) versions. Also see this
        // documentation in the r8 source code:
        // https://r8.googlesource.com/r8/+/2638db4d3465d785a6a740cf09969cab96099cff/src/main/java/com/android/tools/r8/utils/InternalOptions.java#604
        continue;
      }

      m_forward.emplace_back((Forward){earlier_insn, insn});
    }
  }
}

bool CommonSubexpressionElimination::patch(bool is_static,
                                           DexType* declaring_type,
                                           DexTypeList* args,
                                           bool runtime_assertions) {
  if (m_forward.size() == 0) {
    return false;
  }

  TRACE(CSE, 5, "[CSE] before:\n%s", SHOW(m_cfg));

  // gather relevant instructions, and allocate temp registers

  std::unordered_map<IRInstruction*, std::pair<IROpcode, uint32_t>> temps;
  std::unordered_set<IRInstruction*> insns;
  for (auto& f : m_forward) {
    IRInstruction* earlier_insn = f.earlier_insn;
    if (!temps.count(earlier_insn)) {
      uint32_t src_reg;
      IROpcode move_opcode;
      if (earlier_insn->has_dest()) {
        src_reg = earlier_insn->dest();
        move_opcode = earlier_insn->dest_is_wide()
                          ? OPCODE_MOVE_WIDE
                          : earlier_insn->dest_is_object() ? OPCODE_MOVE_OBJECT
                                                           : OPCODE_MOVE;
        m_stats.results_captured++;
      } else if (earlier_insn->opcode() == OPCODE_NEW_ARRAY) {
        src_reg = earlier_insn->src(0);
        move_opcode = OPCODE_MOVE;
        m_stats.array_lengths_captured++;
      } else {
        always_assert(is_aput(earlier_insn->opcode()) ||
                      is_iput(earlier_insn->opcode()) ||
                      is_sput(earlier_insn->opcode()));
        src_reg = earlier_insn->src(0);
        move_opcode = earlier_insn->src_is_wide(0)
                          ? OPCODE_MOVE_WIDE
                          : (earlier_insn->opcode() == OPCODE_APUT_OBJECT ||
                             earlier_insn->opcode() == OPCODE_IPUT_OBJECT ||
                             earlier_insn->opcode() == OPCODE_SPUT_OBJECT)
                                ? OPCODE_MOVE_OBJECT
                                : OPCODE_MOVE;
        m_stats.stores_captured++;
      }
      uint32_t temp_reg = move_opcode == OPCODE_MOVE_WIDE
                              ? m_cfg.allocate_wide_temp()
                              : m_cfg.allocate_temp();
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

  std::vector<std::pair<Forward, IRInstruction*>> to_check;
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

    if (runtime_assertions) {
      to_check.emplace_back(f, move_insn);
    }

    TRACE(CSE, 4, "[CSE] forwarding %s to %s via v%u", SHOW(earlier_insn),
          SHOW(insn), temp_reg);

    if (opcode::is_move_result_any(insn->opcode())) {
      insn = m_cfg.primary_instruction_of_move_result(it)->insn;
      if (is_invoke(insn->opcode())) {
        TRACE(CSE, 3, "[CSE] eliminating invocation of %s",
              SHOW(insn->get_method()));
      }
    }
    m_stats.eliminated_opcodes[insn->opcode()]++;
  }

  // insert moves to define the forwarded value

  for (auto& r : temps) {
    IRInstruction* earlier_insn = r.first;
    IROpcode move_opcode = r.second.first;
    uint32_t temp_reg = r.second.second;

    auto& it = iterators.at(earlier_insn);
    IRInstruction* move_insn = new IRInstruction(move_opcode);
    auto src_reg =
        earlier_insn->has_dest() ? earlier_insn->dest() : earlier_insn->src(0);
    move_insn->set_src(0, src_reg)->set_dest(temp_reg);
    if (earlier_insn->opcode() == OPCODE_NEW_ARRAY) {
      // we need to capture the array-length register of a new-array instruction
      // *before* the instruction, as the dest of the instruction may overwrite
      // the incoming array length value
      m_cfg.insert_before(it, move_insn);
    } else {
      m_cfg.insert_after(it, move_insn);
    }
  }

  if (runtime_assertions) {
    insert_runtime_assertions(is_static, declaring_type, args, to_check);
  }

  TRACE(CSE, 5, "[CSE] after:\n%s", SHOW(m_cfg));

  m_stats.instructions_eliminated += m_forward.size();
  return true;
}

void CommonSubexpressionElimination::insert_runtime_assertions(
    bool is_static,
    DexType* declaring_type,
    DexTypeList* args,
    const std::vector<std::pair<Forward, IRInstruction*>>& to_check) {
  // For every instruction that CSE will effectively eliminate, we insert
  // code like the following:
  //
  // OLD_CODE:
  //    first-instruction r0
  //    redundant-instruction r1
  //  NEW_ASSERTION_CODE:
  //    if-ne r0, r1, THROW
  //  CSE_CODE:
  //    move r1, r0 // <= to realize CSE; without the NEW_ASSERTION_CODE,
  //                //    the redundant-instruction would be eliminated by DCE.
  //    ...
  //  THROW:
  //    const r2, 0
  //    throw r2
  //
  // The new throw instruction would throw a NullPointerException when the
  // redundant instruction didn't actually produce the same result as the
  // first instruction.
  //
  // TODO: Consider throwing a custom exception, possibly created by code
  // sitting behind an auxiliary method call to keep the code size distortion
  // small which may influence other optimizations. See
  // ConstantPropagationAssertHandler for inspiration.
  //
  // Note: Somehow inserting assertions seems to trip up the OptimizeEnumsPass
  // (it fails while running Redex).
  // TODO: Investigate why. Until then, disable that pass to test CSE.

  // If the original block had a throw-edge, then the new block that throws an
  // exception needs to have a corresponding throw-edge. As we split blocks to
  // insert conditional branches, and splitting blocks removes throw-edges from
  // the original block, we need to make sure that we track what throw-edges as\
  // needed. (All this is to appease the Android verifier in the presence of
  // monitor instructions.)
  std::unordered_map<cfg::Block*, std::vector<cfg::Edge*>> outgoing_throws;
  for (auto b : m_cfg.blocks()) {
    outgoing_throws.emplace(b, b->get_outgoing_throws_in_order());
  }

  // We need type inference information to generate the right kinds of
  // conditional branches.
  type_inference::TypeInference type_inference(m_cfg);
  type_inference.run(is_static, declaring_type, args);
  auto& type_environments = type_inference.get_type_environments();

  for (auto& p : to_check) {
    auto& f = p.first;
    IRInstruction* earlier_insn = f.earlier_insn;
    IRInstruction* insn = f.insn;
    auto move_insn = p.second;

    auto& type_environment = type_environments.at(insn);
    auto temp = move_insn->src(0);
    auto type = type_environment.get_type(temp);
    always_assert(!type.is_top());
    always_assert(!type.is_bottom());
    TRACE(CSE, 6, "[CSE] to check: %s => %s - r%u: %s", SHOW(earlier_insn),
          SHOW(insn), temp, SHOW(type.element()));
    always_assert(type.element() != CONST2);
    always_assert(type.element() != LONG2);
    always_assert(type.element() != DOUBLE2);
    always_assert(type.element() != SCALAR2);
    if (type.element() != ZERO && type.element() != CONST &&
        type.element() != INT && type.element() != REFERENCE &&
        type.element() != LONG1) {
      // TODO: Handle floats and doubles via Float.floatToIntBits and
      // Double.doubleToLongBits to deal with NaN.
      // TODO: Improve TypeInference so that we never have to deal with
      // SCALAR* values where we don't know if it's a int/float or long/double.
      continue;
    }

    auto it = m_cfg.find_insn(insn);
    auto old_block = it.block();
    auto new_block = m_cfg.split_block(it);
    outgoing_throws.emplace(new_block, outgoing_throws.at(old_block));

    auto throw_block = m_cfg.create_block();
    auto null_reg = m_cfg.allocate_temp();
    IRInstruction* const_insn = new IRInstruction(OPCODE_CONST);
    const_insn->set_literal(0);
    const_insn->set_dest(null_reg);
    throw_block->push_back(const_insn);
    IRInstruction* throw_insn = new IRInstruction(OPCODE_THROW);
    throw_insn->set_src(0, null_reg);
    throw_block->push_back(throw_insn);

    for (auto e : outgoing_throws.at(old_block)) {
      auto throw_info = e->throw_info();
      m_cfg.add_edge(throw_block, e->target(), throw_info->catch_type,
                     throw_info->index);
    }

    if (type.element() == LONG1) {
      auto cmp_reg = m_cfg.allocate_temp();
      IRInstruction* cmp_insn = new IRInstruction(OPCODE_CMP_LONG);
      cmp_insn->set_dest(cmp_reg);
      cmp_insn->set_src(0, move_insn->dest());
      cmp_insn->set_src(1, move_insn->src(0));
      old_block->push_back(cmp_insn);

      IRInstruction* if_insn = new IRInstruction(OPCODE_IF_NEZ);
      if_insn->set_src(0, cmp_reg);
      m_cfg.create_branch(old_block, if_insn, new_block, throw_block);
    } else {
      IRInstruction* if_insn = new IRInstruction(OPCODE_IF_NE);
      if_insn->set_src(0, move_insn->dest());
      if_insn->set_src(1, move_insn->src(0));
      m_cfg.create_branch(old_block, if_insn, new_block, throw_block);
    }
  }
}

} // namespace cse_impl

void CommonSubexpressionEliminationPass::bind_config() {
  bind("debug", false, m_debug);
  bind("runtime_assertions", false, m_runtime_assertions);
  // The default value 77 is somewhat arbitrary. In practice, the fixed-point
  // computation terminates after fewer iterations.
  int64_t default_max_iterations = 77;
  bind("max_iterations", default_max_iterations, m_max_iterations);
  after_configuration([this] { always_assert(m_max_iterations >= 0); });
}

void CommonSubexpressionEliminationPass::run_pass(DexStoresVector& stores,
                                                  ConfigFiles& conf,
                                                  PassManager& mgr) {
  const auto scope = build_class_scope(stores);

  auto pure_methods = /* Android framework */ get_pure_methods();
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());

  auto shared_state = SharedState(pure_methods);
  auto method_barriers_stats =
      shared_state.init_method_barriers(scope, m_max_iterations);
  const auto stats = walk::parallel::reduce_methods<Stats>(
      scope,
      [&](DexMethod* method) {
        const auto code = method->get_code();
        if (code == nullptr || method->rstate.no_optimizations()) {
          return Stats();
        }

        TRACE(CSE, 3, "[CSE] processing %s", SHOW(method));
        always_assert(code->editable_cfg_built());
        CommonSubexpressionElimination cse(&shared_state, code->cfg());
        bool any_changes =
            cse.patch(is_static(method), method->get_class(),
                      method->get_proto()->get_args(), m_runtime_assertions);
        code->clear_cfg();
        if (any_changes) {
          // TODO: CopyPropagation and LocalDce will separately construct
          // an editable cfg. Don't do that, and fully convert those passes
          // to be cfg-based.

          CopyPropagationPass::Config config;
          copy_propagation_impl::CopyPropagation copy_propagation(config);
          copy_propagation.run(code, method);

          auto local_dce = LocalDce(pure_methods);
          local_dce.dce(code);

          if (traceEnabled(CSE, 5)) {
            code->build_cfg(/* editable */ true);
            TRACE(CSE, 5, "[CSE] final:\n%s", SHOW(code->cfg()));
            code->clear_cfg();
          }
        }
        return cse.get_stats();
      },
      [](Stats a, Stats b) {
        a.results_captured += b.results_captured;
        a.stores_captured += b.stores_captured;
        a.array_lengths_captured += b.array_lengths_captured;
        a.instructions_eliminated += b.instructions_eliminated;
        a.max_value_ids = std::max(a.max_value_ids, b.max_value_ids);
        a.methods_using_other_tracked_location_bit +=
            b.methods_using_other_tracked_location_bit;
        for (auto& p : b.eliminated_opcodes) {
          a.eliminated_opcodes[p.first] += p.second;
        }
        return a;
      },
      Stats{},
      m_debug ? 1 : redex_parallel::default_num_threads());
  mgr.incr_metric(METRIC_RESULTS_CAPTURED, stats.results_captured);
  mgr.incr_metric(METRIC_STORES_CAPTURED, stats.stores_captured);
  mgr.incr_metric(METRIC_ARRAY_LENGTHS_CAPTURED, stats.array_lengths_captured);
  mgr.incr_metric(METRIC_ELIMINATED_INSTRUCTIONS,
                  stats.instructions_eliminated);
  mgr.incr_metric(METRIC_INLINED_BARRIERS_INTO_METHODS,
                  method_barriers_stats.inlined_barriers_into_methods);
  mgr.incr_metric(METRIC_INLINED_BARRIERS_ITERATIONS,
                  method_barriers_stats.inlined_barriers_iterations);
  mgr.incr_metric(METRIC_MAX_VALUE_IDS, stats.max_value_ids);
  mgr.incr_metric(METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT,
                  stats.methods_using_other_tracked_location_bit);
  for (auto& p : stats.eliminated_opcodes) {
    std::string name = METRIC_INSTR_PREFIX;
    name += SHOW(static_cast<IROpcode>(p.first));
    mgr.incr_metric(name, p.second);
  }

  shared_state.cleanup();
}

static CommonSubexpressionEliminationPass s_pass;
