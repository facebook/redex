/*
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
 */

#include "CommonSubexpressionElimination.h"

#include <utility>

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "HashedSetAbstractDomain.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
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
constexpr const char* METRIC_MAX_VALUE_IDS = "max_value_ids";
constexpr const char* METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT =
    "methods_using_other_tracked_location_bit";
constexpr const char* METRIC_INSTR_PREFIX = "instr_";
constexpr const char* METRIC_METHOD_BARRIERS = "num_method_barriers";
constexpr const char* METRIC_METHOD_BARRIERS_ITERATIONS =
    "num_method_barriers_iterations";
constexpr const char* METRIC_CONDITIONALLY_PURE_METHODS =
    "num_conditionally_pure_methods";
constexpr const char* METRIC_CONDITIONALLY_PURE_METHODS_ITERATIONS =
    "num_conditionally_pure_methods_iterations";
constexpr const char* METRIC_MAX_ITERATIONS = "num_max_iterations";

using value_id_t = uint64_t;
constexpr size_t TRACKED_LOCATION_BITS = 42; // leaves 20 bits for running index
enum ValueIdFlags : value_id_t {
  // lower bits for tracked locations
  UNTRACKED = 0,
  IS_FIRST_TRACKED_LOCATION = ((value_id_t)1),
  IS_OTHER_TRACKED_LOCATION = ((value_id_t)1) << TRACKED_LOCATION_BITS,
  IS_ONLY_READ_NOT_WRITTEN_LOCATION = ((value_id_t)1)
                                      << (TRACKED_LOCATION_BITS + 1),
  IS_TRACKED_LOCATION_MASK = IS_ONLY_READ_NOT_WRITTEN_LOCATION * 2 - 1,
  // upper bits for unique running index
  BASE = ((value_id_t)1) << (TRACKED_LOCATION_BITS + 2),
};

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

using IRInstructionsDomain =
    sparta::PatriciaTreeSetAbstractDomain<const IRInstruction*>;
using ValueIdDomain = sparta::ConstantAbstractDomain<value_id_t>;
using DefEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<value_id_t,
                                               IRInstructionsDomain>;
using RefEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, ValueIdDomain>;

class CseEnvironment final
    : public sparta::ReducedProductAbstractDomain<CseEnvironment,
                                                  DefEnvironment,
                                                  RefEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;
  CseEnvironment()
      : ReducedProductAbstractDomain(
            std::make_tuple(DefEnvironment(), RefEnvironment())) {}

  static void reduce_product(std::tuple<DefEnvironment, RefEnvironment>&) {}

  const DefEnvironment& get_def_env() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const RefEnvironment& get_ref_env() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  CseEnvironment& mutate_def_env(std::function<void(DefEnvironment*)> f) {
    apply<0>(std::move(f));
    return *this;
  }

  CseEnvironment& mutate_ref_env(std::function<void(RefEnvironment*)> f) {
    apply<1>(std::move(f));
    return *this;
  }
};

static Barrier make_barrier(const IRInstruction* insn) {
  Barrier b;
  b.opcode = insn->opcode();
  if (insn->has_field()) {
    b.field =
        resolve_field(insn->get_field(), opcode::is_an_sfield_op(insn->opcode())
                                             ? FieldSearch::Static
                                             : FieldSearch::Instance);
  } else if (insn->has_method()) {
    b.method = resolve_method(insn->get_method(), opcode_to_search(insn));
  }
  return b;
}

CseLocation get_written_array_location(IROpcode opcode) {
  switch (opcode) {
  case OPCODE_APUT:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_INT);
  case OPCODE_APUT_BYTE:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_BYTE);
  case OPCODE_APUT_CHAR:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_CHAR);
  case OPCODE_APUT_WIDE:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_WIDE);
  case OPCODE_APUT_SHORT:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_SHORT);
  case OPCODE_APUT_OBJECT:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_OBJECT);
  case OPCODE_APUT_BOOLEAN:
    return CseLocation(CseSpecialLocations::ARRAY_COMPONENT_TYPE_BOOLEAN);
  default:
    always_assert(false);
  }
}

CseLocation get_written_location(const Barrier& barrier) {
  if (opcode::is_an_aput(barrier.opcode)) {
    return get_written_array_location(barrier.opcode);
  } else if (opcode::is_an_iput(barrier.opcode) ||
             opcode::is_an_sput(barrier.opcode)) {
    return get_field_location(barrier.opcode, barrier.field);
  } else {
    return CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER);
  }
}

bool is_barrier_relevant(const Barrier& barrier,
                         const CseUnorderedLocationSet& read_locations) {
  auto location = get_written_location(barrier);
  return location == CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER) ||
         read_locations.count(location);
}

const CseUnorderedLocationSet no_locations = CseUnorderedLocationSet();

const CseUnorderedLocationSet general_memory_barrier_locations =
    CseUnorderedLocationSet{
        CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER)};

class Analyzer final : public BaseIRAnalyzer<CseEnvironment> {
 public:
  Analyzer(SharedState* shared_state,
           cfg::ControlFlowGraph& cfg,
           bool is_method_static,
           bool is_method_init_or_clinit,
           DexType* declaring_type)
      : BaseIRAnalyzer(cfg), m_shared_state(shared_state) {
    // Collect all read locations
    std::unordered_map<CseLocation, size_t, CseLocationHasher>
        read_location_counts;
    for (const auto& mie : cfg::InstructionIterable(cfg)) {
      auto insn = mie.insn;
      auto location = get_read_location(insn);
      if (location !=
          CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER)) {
        read_location_counts[location]++;
      } else if (opcode::is_an_invoke(insn->opcode()) &&
                 shared_state->has_pure_method(insn)) {
        for (auto l :
             shared_state->get_read_locations_of_conditionally_pure_method(
                 insn->get_method(), insn->opcode())) {
          read_location_counts[l]++;
        }
      }
    }

    // Prune those which are final fields that cannot get mutated in our context
    for (auto it = read_location_counts.begin();
         it != read_location_counts.end();) {
      auto location = it->first;
      // If we are reading a final field...
      if (location.has_field() && is_final(location.get_field()) &&
          !root(location.get_field()) && can_rename(location.get_field()) &&
          can_delete(location.get_field()) &&
          !location.get_field()->is_external()) {
        // ... and we are not analyzing a method that is a corresponding
        // constructor or static initializer of the declaring type of the
        // field ...
        if (!is_method_init_or_clinit ||
            location.get_field()->get_class() != declaring_type ||
            is_static(location.get_field()) != is_method_static) {
          // ... then we don't need track the field as a memory location
          // (that in turn might get invalidated on general memory barriers).
          m_tracked_locations.emplace(location, ValueIdFlags::UNTRACKED);
          it = read_location_counts.erase(it);
          continue;
        }
      }
      m_read_locations.insert(location);
      it++;
    }

    // Collect all relevant written locations
    std::unordered_map<CseLocation, size_t, CseLocationHasher>
        written_location_counts;
    for (const auto& mie : cfg::InstructionIterable(cfg)) {
      auto locations = shared_state->get_relevant_written_locations(
          mie.insn, nullptr /* exact_virtual_scope */, m_read_locations);
      if (!locations.count(
              CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER))) {
        for (const auto& location : locations) {
          written_location_counts[location]++;
        }
      }
    }

    // Check which locations get written and read (vs. just written)
    std::vector<CseLocation> read_and_written_locations;
    for (const auto& p : written_location_counts) {
      always_assert(p.first !=
                    CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER));
      if (read_location_counts.count(p.first)) {
        read_and_written_locations.push_back(p.first);
      } else {
        always_assert(!m_tracked_locations.count(p.first));
        m_tracked_locations.emplace(p.first, ValueIdFlags::UNTRACKED);
      }
    }

    // Also keep track of locations that get read but not written
    for (const auto& p : read_location_counts) {
      if (!written_location_counts.count(p.first)) {
        always_assert(!m_tracked_locations.count(p.first));
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
              read_and_written_locations.end(),
              [&](CseLocation a, CseLocation b) {
                auto get_weight = [&](CseLocation l) {
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
            l.special_location < CseSpecialLocations::END ? "array element"
                                                          : SHOW(l.field),
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

  void analyze_instruction(const IRInstruction* insn,
                           CseEnvironment* current_state) const override {
    const auto set_current_state_at = [&](reg_t reg, bool wide,
                                          ValueIdDomain value) {
      current_state->mutate_ref_env([&](RefEnvironment* env) {
        env->set(reg, value);
        if (wide) {
          env->set(reg + 1, ValueIdDomain::top());
        }
      });
    };

    init_pre_state(insn, current_state);
    auto clobbered_locations = get_clobbered_locations(insn, current_state);
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
          domain =
              get_value_id_domain(insn, current_state, clobbered_locations);
        }
        auto c = domain.get_constant();
        if (c) {
          auto value_id = *c;
          if (current_state->get_def_env().get(value_id).is_top()) {
            current_state->mutate_def_env([&](DefEnvironment* env) {
              env->set(value_id, IRInstructionsDomain(insn));
            });
          }
        }
        set_current_state_at(insn->dest(), insn->dest_is_wide(), domain);
      } else if (insn->has_move_result_any()) {
        ValueIdDomain domain =
            get_value_id_domain(insn, current_state, clobbered_locations);
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

    if (!clobbered_locations.empty()) {
      value_id_t mask = (value_id_t)0;
      for (const auto& l : clobbered_locations) {
        mask |= get_location_value_id_mask(l);
      }

      // TODO: The following loops are probably the most expensive thing in this
      // algorithm; is there a better way of doing this? (Then again, overall,
      // the time this algorithm takes seems reasonable.)

      bool any_changes = false;
      current_state->mutate_def_env([mask, &any_changes](DefEnvironment* env) {
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

      if (!clobbered_locations.count(
              CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER))) {
        auto value = get_equivalent_put_value(insn, current_state);
        if (value) {
          TRACE(CSE, 4, "[CSE] installing store-to-load forwarding for %s",
                SHOW(insn));
          install_forwarding(insn, *value, current_state);
        }
      }
    }
  }

  void install_forwarding(const IRInstruction* insn,
                          const IRValue& value,
                          CseEnvironment* current_state) const {
    auto value_id = *get_value_id(value);
    current_state->mutate_def_env([value_id, insn](DefEnvironment* env) {
      env->set(value_id, IRInstructionsDomain(insn));
    });
  }

  bool is_pre_state_src(value_id_t value_id) const {
    return !!m_pre_state_value_ids.count(value_id);
  }

  size_t get_value_ids_size() { return m_value_ids.size(); }

  bool using_other_tracked_location_bit() {
    return m_using_other_tracked_location_bit;
  }

 private:
  CseUnorderedLocationSet get_clobbered_locations(
      const IRInstruction* insn, CseEnvironment* current_state) const {
    DexType* exact_virtual_scope = nullptr;
    if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
      auto src0 = current_state->get_ref_env().get(insn->src(0)).get_constant();
      if (src0) {
        exact_virtual_scope = get_exact_type(*src0);
      }
    }
    return m_shared_state->get_relevant_written_locations(
        insn, exact_virtual_scope, m_read_locations);
  }

  ValueIdDomain get_value_id_domain(
      const IRInstruction* insn,
      CseEnvironment* current_state,
      const CseUnorderedLocationSet& clobbered_locations) const {
    auto value = get_value(insn, current_state, clobbered_locations);
    auto value_id = get_value_id(value);
    return value_id ? ValueIdDomain(*value_id) : ValueIdDomain::top();
  }

  value_id_t get_pre_state_src_value_id(reg_t reg,
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
    if (opcode::is_an_aget(value.opcode)) {
      id |= get_location_value_id_mask(get_read_array_location(value.opcode));
    } else if (opcode::is_an_iget(value.opcode) ||
               opcode::is_an_sget(value.opcode)) {
      auto location = get_field_location(value.opcode, value.field);
      if (location ==
          CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER)) {
        return boost::none;
      }
      id |= get_location_value_id_mask(location);
    } else if (opcode::is_an_invoke(value.opcode)) {
      id |= get_invoke_value_id_mask(value);
    }
    if (value.opcode != IOPCODE_PRE_STATE_SRC) {
      for (auto src : value.srcs) {
        id |= (src & ValueIdFlags::IS_TRACKED_LOCATION_MASK);
      }
    }
    m_value_ids.emplace(value, id);
    if (value.opcode == IOPCODE_POSITIONAL) {
      m_positional_insns.emplace(id, value.positional_insn);
    } else if (value.opcode == IOPCODE_PRE_STATE_SRC) {
      m_pre_state_value_ids.insert(id);
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
      const IRInstruction* insn, CseEnvironment* current_state) const {
    const auto& ref_env = current_state->get_ref_env();
    if (opcode::is_an_sput(insn->opcode())) {
      always_assert(insn->srcs_size() == 1);
      IRValue value;
      value.opcode = (IROpcode)(insn->opcode() - OPCODE_SPUT + OPCODE_SGET);
      value.field = insn->get_field();
      return value;
    } else if (opcode::is_an_iput(insn->opcode())) {
      always_assert(insn->srcs_size() == 2);
      auto src1 = ref_env.get(insn->src(1)).get_constant();
      if (src1) {
        IRValue value;
        value.opcode = (IROpcode)(insn->opcode() - OPCODE_IPUT + OPCODE_IGET);
        value.srcs.push_back(*src1);
        value.field = insn->get_field();
        return value;
      }
    } else if (insn->opcode() == OPCODE_APUT_OBJECT) {
      // Skip this case. Statically, the incoming value can be of any object
      // type, as runtime validation ensures type correctness. Thus, we cannot
      // propagate an aput-object to an aget-object with a simple move-object.
      // TODO: Allow this here, but also insert a check-cast instead of a simple
      // move-object in this case, using type inference on the array argument of
      // the aget-object instruction.
    } else if (opcode::is_an_aput(insn->opcode())) {
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

  IRValue get_pre_state_src_value(reg_t reg, const IRInstruction* insn) const {
    IRValue value;
    value.opcode = IOPCODE_PRE_STATE_SRC;
    value.srcs.push_back(reg);
    value.positional_insn = insn;
    return value;
  }

  void init_pre_state(const IRInstruction* insn,
                      CseEnvironment* current_state) const {
    auto ref_env = current_state->get_ref_env();
    std::unordered_map<reg_t, value_id_t> new_pre_state_src_values;
    for (auto reg : insn->srcs()) {
      auto c = ref_env.get(reg).get_constant();
      if (!c) {
        auto it = new_pre_state_src_values.find(reg);
        if (it == new_pre_state_src_values.end()) {
          auto value_id = get_pre_state_src_value_id(reg, insn);
          new_pre_state_src_values.emplace(reg, value_id);
        }
      }
    }
    if (!new_pre_state_src_values.empty()) {
      current_state->mutate_ref_env([&](RefEnvironment* env) {
        for (const auto& p : new_pre_state_src_values) {
          env->set(p.first, ValueIdDomain(p.second));
        }
      });
    }
  }

  IRValue get_value(const IRInstruction* insn,
                    CseEnvironment* current_state,
                    const CseUnorderedLocationSet& clobbered_locations) const {
    IRValue value;
    auto opcode = insn->opcode();
    always_assert(opcode != IOPCODE_PRE_STATE_SRC);
    value.opcode = opcode;
    const auto& ref_env = current_state->get_ref_env();
    for (auto reg : insn->srcs()) {
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
      // there might be an impacted field, array element, general memory barrier
      always_assert(clobbered_locations.size() <= 1);
      is_positional = !clobbered_locations.empty();
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

  value_id_t get_location_value_id_mask(CseLocation l) const {
    if (l == CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER)) {
      return ValueIdFlags::IS_TRACKED_LOCATION_MASK;
    } else {
      return m_tracked_locations.at(l);
    }
  }

  value_id_t get_invoke_value_id_mask(const IRValue& value) const {
    always_assert(opcode::is_an_invoke(value.opcode));
    value_id_t mask = 0;
    for (auto l :
         m_shared_state->get_read_locations_of_conditionally_pure_method(
             value.method, value.opcode)) {
      mask |= get_location_value_id_mask(l);
    }
    return mask;
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
  CseUnorderedLocationSet m_read_locations;
  std::unordered_map<CseLocation, value_id_t, CseLocationHasher>
      m_tracked_locations;
  SharedState* m_shared_state;
  mutable std::unordered_map<IRValue, value_id_t, IRValueHasher> m_value_ids;
  mutable std::unordered_set<value_id_t> m_pre_state_value_ids;
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

  if (traceEnabled(CSE, 2)) {
    m_barriers.reset(new ConcurrentMap<Barrier, size_t, BarrierHasher>());
  }
}

const method_override_graph::Graph* SharedState::get_method_override_graph()
    const {
  return m_method_override_graph.get();
}

void SharedState::init_method_barriers(const Scope& scope) {
  Timer t("init_method_barriers");
  auto iterations = compute_locations_closure(
      scope, m_method_override_graph.get(),
      [&](DexMethod* method) -> boost::optional<LocationsAndDependencies> {
        auto action = get_base_or_overriding_method_action(
            method, &m_safe_method_defs,
            /* ignore_methods_with_assumenosideeffects */ true);
        if (action == MethodOverrideAction::UNKNOWN) {
          return boost::none;
        }
        LocationsAndDependencies lads;
        if (action == MethodOverrideAction::EXCLUDE) {
          return lads;
        }
        auto code = method->get_code();
        for (const auto& mie : cfg::InstructionIterable(code->cfg())) {
          auto* insn = mie.insn;
          if (may_be_barrier(insn, nullptr /* exact_virtual_scope */)) {
            auto barrier = make_barrier(insn);
            if (!opcode::is_an_invoke(barrier.opcode)) {
              auto location = get_written_location(barrier);
              if (location ==
                  CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER)) {
                return boost::none;
              }
              lads.locations.insert(location);
              continue;
            }

            if (barrier.opcode == OPCODE_INVOKE_SUPER) {
              // TODO: Implement
              return boost::none;
            }

            if (!process_base_and_overriding_methods(
                    m_method_override_graph.get(), barrier.method,
                    &m_safe_method_defs,
                    /* ignore_methods_with_assumenosideeffects */ true,
                    [&](DexMethod* other_method) {
                      if (other_method != method) {
                        lads.dependencies.insert(other_method);
                      }
                      return true;
                    })) {
              return boost::none;
            }
          }
        }

        return lads;
      },
      &m_method_written_locations);
  m_stats.method_barriers_iterations = iterations;
  m_stats.method_barriers = m_method_written_locations.size();

  for (const auto& p : m_method_written_locations) {
    auto method = p.first;
    auto& written_locations = p.second;
    TRACE(CSE, 4, "[CSE] inferred barrier for %s: %s", SHOW(method),
          SHOW(&written_locations));
  }
}

void SharedState::init_scope(const Scope& scope) {
  always_assert(!m_method_override_graph);
  m_method_override_graph = method_override_graph::build_graph(scope);

  auto iterations = compute_conditionally_pure_methods(
      scope, m_method_override_graph.get(), m_pure_methods,
      &m_conditionally_pure_methods);
  m_stats.conditionally_pure_methods = m_conditionally_pure_methods.size();
  m_stats.conditionally_pure_methods_iterations = iterations;
  for (const auto& p : m_conditionally_pure_methods) {
    m_pure_methods.insert(const_cast<DexMethod*>(p.first));
  }

  for (auto method_ref : m_safe_methods) {
    auto method = method_ref->as_def();
    if (method) {
      m_safe_method_defs.insert(method);
    }
  }

  init_method_barriers(scope);
}

CseUnorderedLocationSet SharedState::get_relevant_written_locations(
    const IRInstruction* insn,
    DexType* exact_virtual_scope,
    const CseUnorderedLocationSet& read_locations) {
  if (may_be_barrier(insn, exact_virtual_scope)) {
    if (opcode::is_an_invoke(insn->opcode())) {
      return get_relevant_written_locations(insn, read_locations);
    } else {
      auto barrier = make_barrier(insn);
      if (is_barrier_relevant(barrier, read_locations)) {
        return CseUnorderedLocationSet{get_written_location(barrier)};
      }
    }
  }
  return no_locations;
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
    if (opcode::is_an_aput(opcode) || opcode::is_an_iput(opcode) ||
        opcode::is_an_sput(opcode)) {
      return true;
    } else if (opcode::is_an_invoke(opcode)) {
      return !is_invoke_safe(insn, exact_virtual_scope);
    }
    if (insn->has_field()) {
      always_assert(opcode::is_an_iget(opcode) || opcode::is_an_sget(opcode));
      if (get_field_location(opcode, insn->get_field()) ==
          CseLocation(CseSpecialLocations::GENERAL_MEMORY_BARRIER)) {
        return true;
      }
    }

    return false;
  }
}

bool SharedState::is_invoke_safe(const IRInstruction* insn,
                                 DexType* exact_virtual_scope) {
  always_assert(opcode::is_an_invoke(insn->opcode()));
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

  if (opcode == OPCODE_INVOKE_INTERFACE && m_safe_methods.count(method)) {
    return true;
  }

  return false;
}

CseUnorderedLocationSet SharedState::get_relevant_written_locations(
    const IRInstruction* insn, const CseUnorderedLocationSet& read_locations) {
  always_assert(opcode::is_an_invoke(insn->opcode()));

  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_SUPER) {
    // TODO
    return general_memory_barrier_locations;
  }

  auto method_ref = insn->get_method();
  DexMethod* method = resolve_method(method_ref, opcode_to_search(insn));
  CseUnorderedLocationSet written_locations;
  if (!process_base_and_overriding_methods(
          m_method_override_graph.get(), method, &m_safe_method_defs,
          /* ignore_methods_with_assumenosideeffects */ true,
          [&](DexMethod* other_method) {
            auto it = m_method_written_locations.find(other_method);
            if (it == m_method_written_locations.end()) {
              return false;
            }
            written_locations.insert(it->second.begin(), it->second.end());
            return true;
          })) {
    return general_memory_barrier_locations;
  }

  // Remove written locations that are not read
  for (auto it = written_locations.begin(); it != written_locations.end();) {
    if (!read_locations.count(*it)) {
      it = written_locations.erase(it);
    } else {
      it++;
    }
  }

  return written_locations;
}

void SharedState::log_barrier(const Barrier& barrier) {
  if (m_barriers) {
    m_barriers->update(
        barrier, [](const Barrier, size_t& v, bool /* exists */) { v++; });
  }
}

const CseUnorderedLocationSet&
SharedState::get_read_locations_of_conditionally_pure_method(
    const DexMethodRef* method_ref, IROpcode opcode) const {
  auto method = resolve_method(const_cast<DexMethodRef*>(method_ref),
                               opcode_to_search(opcode));
  if (method == nullptr) {
    return no_locations;
  }
  auto it = m_conditionally_pure_methods.find(method);
  if (it == m_conditionally_pure_methods.end()) {
    return no_locations;
  } else {
    return it->second;
  }
}

bool SharedState::has_pure_method(const IRInstruction* insn) const {
  auto method_ref = insn->get_method();
  if (m_pure_methods.find(method_ref) != m_pure_methods.end()) {
    TRACE(CSE, 4, "[CSE] unresolved %spure for %s",
          (method_ref->is_def() &&
           m_conditionally_pure_methods.count(method_ref->as_def()))
              ? "conditionally "
              : "",
          SHOW(method_ref));
    return true;
  }

  auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
  if (method != nullptr &&
      m_pure_methods.find(method) != m_pure_methods.end()) {
    TRACE(CSE, 4, "[CSE] resolved %spure for %s",
          m_conditionally_pure_methods.count(method) ? "conditionally " : "",
          SHOW(method));
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
  for (const auto& p : ordered_barriers) {
    auto& b = p.first;
    auto c = p.second;
    if (opcode::is_an_invoke(b.opcode)) {
      TRACE(CSE, 2, "%s %s x %u", SHOW(b.opcode), SHOW(b.method), c);
    } else if (opcode::is_an_ifield_op(b.opcode) ||
               opcode::is_an_sfield_op(b.opcode)) {
      TRACE(CSE, 2, "%s %s x %u", SHOW(b.opcode), SHOW(b.field), c);
    } else {
      TRACE(CSE, 2, "%s x %u", SHOW(b.opcode), c);
    }
  }
}

CommonSubexpressionElimination::CommonSubexpressionElimination(
    SharedState* shared_state,
    cfg::ControlFlowGraph& cfg,
    bool is_static,
    bool is_init_or_clinit,
    DexType* declaring_type,
    DexTypeList* args)
    : m_shared_state(shared_state),
      m_cfg(cfg),
      m_is_static(is_static),
      m_declaring_type(declaring_type),
      m_args(args) {
  Analyzer analyzer(shared_state, cfg, is_static, is_init_or_clinit,
                    declaring_type);
  m_stats.max_value_ids = analyzer.get_value_ids_size();
  if (analyzer.using_other_tracked_location_bit()) {
    m_stats.methods_using_other_tracked_location_bit = 1;
  }

  // We need some helper state/functions to build the list m_earlier_insns
  // of unique earlier-instruction sets. To make that deterministic, we use
  // instruction ids that represent the position of an instruction in the cfg.
  std::unordered_map<const IRInstruction*, size_t> insn_ids;
  for (const auto& mie : InstructionIterable(cfg)) {
    insn_ids.emplace(mie.insn, insn_ids.size());
  }
  std::unordered_map<std::vector<size_t>, size_t,
                     boost::hash<std::vector<size_t>>>
      insns_ids;
  auto get_earlier_insns_index =
      [&](const PatriciaTreeSet<const IRInstruction*>& insns) {
        std::vector<size_t> ordered_ids;
        for (auto insn : insns) {
          ordered_ids.push_back(insn_ids.at(insn));
        }
        std::sort(ordered_ids.begin(), ordered_ids.end());
        auto it = insns_ids.find(ordered_ids);
        if (it != insns_ids.end()) {
          return it->second;
        }
        auto index = insns_ids.size();
        always_assert(m_earlier_insns.size() == index);
        insns_ids.emplace(ordered_ids, index);
        m_earlier_insns.push_back(insns);
        return index;
      };

  // identify all instruction pairs where the result of the first instruction
  // can be forwarded to the second

  for (cfg::Block* block : cfg.blocks()) {
    auto env = analyzer.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    for (const auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      analyzer.analyze_instruction(insn, &env);
      auto opcode = insn->opcode();
      if (!insn->has_dest() || opcode::is_a_move(opcode) ||
          opcode::is_a_const(opcode)) {
        continue;
      }
      auto ref_c = env.get_ref_env().get(insn->dest()).get_constant();
      if (!ref_c) {
        continue;
      }
      auto value_id = *ref_c;
      always_assert(!analyzer.is_pre_state_src(value_id));
      auto defs = env.get_def_env().get(value_id);
      always_assert(!defs.is_top() && !defs.is_bottom());
      auto earlier_insns = defs.elements();
      if (earlier_insns.contains(insn)) {
        continue;
      }
      bool skip{false};
      for (auto earlier_insn : earlier_insns) {
        auto earlier_opcode = earlier_insn->opcode();
        if (opcode::is_a_load_param(earlier_opcode)) {
          skip = true;
          break;
        }
        if (opcode::is_a_cmp(opcode) || opcode::is_a_cmp(earlier_opcode)) {
          // See T46241704. We never de-duplicate cmp instructions due to an
          // apparent bug in various Dalvik (and ART?) versions. Also see this
          // documentation in the r8 source code:
          // https://r8.googlesource.com/r8/+/2638db4d3465d785a6a740cf09969cab96099cff/src/main/java/com/android/tools/r8/utils/InternalOptions.java#604
          skip = true;
          break;
        }
      }
      if (skip) {
        continue;
      }

      auto earlier_insns_index = get_earlier_insns_index(earlier_insns);
      m_forward.push_back({earlier_insns_index, insn});
    }
  }
}

static IROpcode get_move_opcode(const IRInstruction* earlier_insn) {
  if (earlier_insn->has_dest()) {
    return earlier_insn->dest_is_wide()
               ? OPCODE_MOVE_WIDE
               : earlier_insn->dest_is_object() ? OPCODE_MOVE_OBJECT
                                                : OPCODE_MOVE;
  } else if (earlier_insn->opcode() == OPCODE_NEW_ARRAY) {
    return OPCODE_MOVE;
  } else {
    always_assert(opcode::is_an_aput(earlier_insn->opcode()) ||
                  opcode::is_an_iput(earlier_insn->opcode()) ||
                  opcode::is_an_sput(earlier_insn->opcode()));
    return earlier_insn->src_is_wide(0)
               ? OPCODE_MOVE_WIDE
               : (earlier_insn->opcode() == OPCODE_APUT_OBJECT ||
                  earlier_insn->opcode() == OPCODE_IPUT_OBJECT ||
                  earlier_insn->opcode() == OPCODE_SPUT_OBJECT)
                     ? OPCODE_MOVE_OBJECT
                     : OPCODE_MOVE;
  }
}

bool CommonSubexpressionElimination::patch(bool runtime_assertions) {
  if (m_forward.empty()) {
    return false;
  }

  unsigned int max_dest = 0;
  for (const auto& mie : cfg::InstructionIterable(m_cfg)) {
    if (mie.insn->has_dest() && mie.insn->dest() > max_dest) {
      max_dest = mie.insn->dest();
    }
  };
  for (const auto& earlier_insns : m_earlier_insns) {
    IROpcode move_opcode = get_move_opcode(*earlier_insns.begin());
    max_dest += (move_opcode == OPCODE_MOVE_WIDE) ? 2 : 1;
  }

  TRACE(CSE, 5, "[CSE] before:\n%s", SHOW(m_cfg));

  // gather relevant instructions, and allocate temp registers

  // We'll allocate one temp per "earlier_insns_index".
  // TODO: Do better, use less. A subset and its superset can share a temp.
  std::unordered_map<size_t, std::pair<IROpcode, reg_t>> temps;
  // We also remember for which instructions we'll need an iterator, as we'll
  // want to insert something after them.
  std::unordered_set<const IRInstruction*> iterator_insns;
  std::unordered_set<const IRInstruction*> combined_earlier_insns;
  for (const auto& f : m_forward) {
    iterator_insns.insert(f.insn);
    if (temps.count(f.earlier_insns_index)) {
      continue;
    }
    auto& earlier_insns = m_earlier_insns.at(f.earlier_insns_index);
    combined_earlier_insns.insert(earlier_insns.begin(), earlier_insns.end());
    IROpcode move_opcode = get_move_opcode(*earlier_insns.begin());
    reg_t temp_reg = move_opcode == OPCODE_MOVE_WIDE
                         ? m_cfg.allocate_wide_temp()
                         : m_cfg.allocate_temp();
    temps.emplace(f.earlier_insns_index, std::make_pair(move_opcode, temp_reg));
  }
  for (auto earlier_insn : combined_earlier_insns) {
    iterator_insns.insert(earlier_insn);
    if (earlier_insn->has_dest()) {
      m_stats.results_captured++;
    } else if (earlier_insn->opcode() == OPCODE_NEW_ARRAY) {
      m_stats.array_lengths_captured++;
    } else {
      always_assert(opcode::is_an_aput(earlier_insn->opcode()) ||
                    opcode::is_an_iput(earlier_insn->opcode()) ||
                    opcode::is_an_sput(earlier_insn->opcode()));
      m_stats.stores_captured++;
    }
  }

  // find all iterators in one sweep

  std::unordered_map<IRInstruction*, cfg::InstructionIterator> iterators;
  auto iterable = cfg::InstructionIterable(m_cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    auto* insn = it->insn;
    if (iterator_insns.count(insn)) {
      iterators.emplace(insn, it);
    }
  }

  // insert moves to use the forwarded value

  std::vector<std::pair<Forward, IRInstruction*>> to_check;
  for (const auto& f : m_forward) {
    auto& earlier_insns = m_earlier_insns.at(f.earlier_insns_index);
    auto& q = temps.at(f.earlier_insns_index);
    IROpcode move_opcode = q.first;
    reg_t temp_reg = q.second;
    IRInstruction* insn = f.insn;
    auto& it = iterators.at(insn);
    IRInstruction* move_insn = new IRInstruction(move_opcode);
    move_insn->set_src(0, temp_reg)->set_dest(insn->dest());
    m_cfg.insert_after(it, move_insn);

    if (runtime_assertions) {
      to_check.emplace_back(f, move_insn);
    }

    for (auto earlier_insn : earlier_insns) {
      TRACE(CSE, 4, "[CSE] forwarding %s to %s via v%u", SHOW(earlier_insn),
            SHOW(insn), temp_reg);
    }

    if (opcode::is_move_result_any(insn->opcode())) {
      insn = m_cfg.primary_instruction_of_move_result(it)->insn;
      if (opcode::is_an_invoke(insn->opcode())) {
        TRACE(CSE, 3, "[CSE] eliminating invocation of %s",
              SHOW(insn->get_method()));
      }
    }
    m_stats.eliminated_opcodes[insn->opcode()]++;
  }

  // insert moves to define the forwarded value

  for (const auto& r : temps) {
    size_t earlier_insns_index = r.first;
    IROpcode move_opcode = r.second.first;
    reg_t temp_reg = r.second.second;
    auto& earlier_insns = m_earlier_insns.at(earlier_insns_index);
    for (auto earlier_insn : earlier_insns) {
      auto& it = iterators.at(const_cast<IRInstruction*>(earlier_insn));
      IRInstruction* move_insn = new IRInstruction(move_opcode);
      auto src_reg = earlier_insn->has_dest() ? earlier_insn->dest()
                                              : earlier_insn->src(0);
      move_insn->set_src(0, src_reg)->set_dest(temp_reg);
      if (earlier_insn->opcode() == OPCODE_NEW_ARRAY) {
        // we need to capture the array-length register of a new-array
        // instruction *before* the instruction, as the dest of the instruction
        // may overwrite the incoming array length value
        m_cfg.insert_before(it, move_insn);
      } else {
        m_cfg.insert_after(it, move_insn);
      }
    }
  }

  if (runtime_assertions) {
    insert_runtime_assertions(to_check);
  }

  TRACE(CSE, 5, "[CSE] after:\n%s", SHOW(m_cfg));

  m_stats.instructions_eliminated += m_forward.size();
  return true;
}

void CommonSubexpressionElimination::insert_runtime_assertions(
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
  type_inference.run(m_is_static, m_declaring_type, m_args);
  auto& type_environments = type_inference.get_type_environments();

  for (const auto& p : to_check) {
    auto& f = p.first;
    auto& earlier_insns = m_earlier_insns.at(f.earlier_insns_index);
    for (auto earlier_insn : earlier_insns) {
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
        // SCALAR* values where we don't know if it's a int/float or
        // long/double.
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
}

Stats& Stats::operator+=(const Stats& that) {
  results_captured += that.results_captured;
  stores_captured += that.stores_captured;
  array_lengths_captured += that.array_lengths_captured;
  instructions_eliminated += that.instructions_eliminated;
  max_value_ids = std::max(max_value_ids, that.max_value_ids);
  methods_using_other_tracked_location_bit +=
      that.methods_using_other_tracked_location_bit;
  for (const auto& p : that.eliminated_opcodes) {
    eliminated_opcodes[p.first] += p.second;
  }
  max_iterations = std::max(max_iterations, that.max_iterations);
  return *this;
}

} // namespace cse_impl
