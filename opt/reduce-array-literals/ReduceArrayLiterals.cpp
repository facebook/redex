/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReduceArrayLiterals.h"

#include <vector>

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "HashedSetAbstractDomain.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PatriciaTreeMap.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSet.h"
#include "Resolver.h"
#include "Walkers.h"

using namespace sparta;

namespace {

constexpr const char* METRIC_FILLED_ARRAYS = "num_filled_arrays";
constexpr const char* METRIC_FILLED_ARRAY_ELEMENTS =
    "num_filled_array_elements";
constexpr const char* METRIC_REMAINING_WIDE_ARRAYS =
    "num_remaining_wide_arrays";
constexpr const char* METRIC_REMAINING_WIDE_ARRAY_ELEMENTS =
    "num_remaining_wide_array_elements";
constexpr const char* METRIC_REMAINING_LARGE_ARRAYS =
    "num_remaining_large_arrays";
constexpr const char* METRIC_REMAINING_LARGE_ARRAY_ELEMENTS =
    "num_remaining_large_array_elements";
constexpr const char* METRIC_REMAINING_UNIMPLEMENTED_ARRAYS =
    "num_remaining_unimplemented_arrays";
constexpr const char* METRIC_REMAINING_UNIMPLEMENTED_ARRAY_ELEMENTS =
    "num_remaining_unimplemented_array_elements";
constexpr const char* METRIC_REMAINING_BUGGY_ARRAYS =
    "num_remaining_buggy_arrays";
constexpr const char* METRIC_REMAINING_BUGGY_ARRAY_ELEMENTS =
    "num_remaining_buggy_array_elements";

/* A tracked value is...
 * - a 32-bit literal,
 * - or a new-array instruction that was reached with a well-known array length,
 *   and has been followed by a number of aput instructions that initialized the
 *   individual array elements in order, or
 * - some other value.
 */

enum TrackedValueKind : uint32_t { Other, Literal, NewArray };

struct TrackedValue {
  TrackedValueKind kind;
  union {
    int32_t literal; // for kind == Literal
    uint32_t length; // for kind == NewArray
  };
  // The following are only used for kind == NewArray
  IRInstruction* new_array_insn;
  uint32_t aput_insns_size{0};
  PatriciaTreeMap<uint32_t, IRInstruction*> aput_insns{};
  PatriciaTreeSet<IRInstruction*> aput_insns_range{};
};

struct TrackedValueHasher {
  size_t operator()(const TrackedValue& tv) const {
    switch (tv.kind) {
    case TrackedValueKind::Other:
      return std::numeric_limits<size_t>::max();
    case TrackedValueKind::Literal:
      return (size_t)tv.literal;
    case TrackedValueKind::NewArray:
      return tv.length + (size_t)tv.new_array_insn ^ tv.aput_insns_size;
    default:
      always_assert(false);
    }
  }
};

bool operator==(const TrackedValue& a, const TrackedValue b) {
  if (a.kind != b.kind) {
    return false;
  }
  switch (a.kind) {
  case TrackedValueKind::Other:
    return true;
  case TrackedValueKind::Literal:
    return a.literal == b.literal;
  case TrackedValueKind::NewArray:
    return a.length == b.length && a.new_array_insn == b.new_array_insn &&
           a.aput_insns_size == b.aput_insns_size &&
           a.aput_insns == b.aput_insns;
  default:
    always_assert(false);
  }
}

TrackedValue make_other() {
  return (TrackedValue){TrackedValueKind::Other, {0}, nullptr};
}

TrackedValue make_literal(IRInstruction* instr) {
  always_assert(instr->opcode() == OPCODE_CONST);
  always_assert(instr->has_literal());
  return (TrackedValue){
      TrackedValueKind::Literal, {(int32_t)instr->get_literal()}, nullptr};
}

TrackedValue make_array(int32_t length, IRInstruction* instr) {
  always_assert(length >= 0);
  always_assert(instr->opcode() == OPCODE_NEW_ARRAY);
  return (TrackedValue){TrackedValueKind::NewArray, {length}, instr};
}

bool is_new_array(const TrackedValue& tv) {
  return tv.kind == TrackedValueKind::NewArray;
}

bool is_literal(const TrackedValue& tv) {
  return tv.kind == TrackedValueKind::Literal;
}

int64_t get_literal(const TrackedValue& tv) {
  always_assert(is_literal(tv));
  return tv.literal;
}

bool is_next_index(const TrackedValue& tv, int64_t index) {
  always_assert(is_new_array(tv));
  return index == tv.aput_insns_size;
}

bool is_array_literal(const TrackedValue& tv) {
  return is_new_array(tv) && (int64_t)tv.aput_insns_size == tv.length;
}

bool add_element(TrackedValue& array, int64_t index, IRInstruction* aput_insn) {
  always_assert(is_new_array(array));
  always_assert(is_next_index(array, index));
  always_assert(!is_array_literal(array));
  always_assert(aput_insn != nullptr);
  if (array.aput_insns_range.contains(aput_insn)) {
    return false;
  }
  array.aput_insns_size++;
  array.aput_insns_range.insert(aput_insn);
  array.aput_insns.insert_or_assign((uint32_t)index, aput_insn);
  return true;
}

std::vector<IRInstruction*> get_aput_insns(const TrackedValue& array) {
  always_assert(is_array_literal(array));
  std::vector<IRInstruction*> aput_insns;
  aput_insns.reserve(array.length);
  for (uint32_t i = 0; i < array.length; i++) {
    IRInstruction* aput_insn = array.aput_insns.at(i);
    always_assert(aput_insn != nullptr);
    aput_insns.push_back(aput_insn);
  }
  return aput_insns;
}

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

using TrackedDomain =
    sparta::HashedSetAbstractDomain<TrackedValue, TrackedValueHasher>;
using EscapedArrayDomain =
    sparta::ConstantAbstractDomain<std::vector<IRInstruction*>>;

/**
 * For each register that holds a relevant value, keep track of it.
 **/
using TrackedDomainEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<register_t, TrackedDomain>;

class Analyzer final : public BaseIRAnalyzer<TrackedDomainEnvironment> {

 public:
  Analyzer(cfg::ControlFlowGraph& cfg) : BaseIRAnalyzer(cfg) {
    MonotonicFixpointIterator::run(TrackedDomainEnvironment::top());
  }

  void analyze_instruction(
      IRInstruction* insn,
      TrackedDomainEnvironment* current_state) const override {

    const auto set_current_state_at = [&](register_t reg, bool wide,
                                          TrackedDomain value) {
      current_state->set(reg, value);
      if (wide) {
        current_state->set(reg + 1, TrackedDomain::top());
      }
    };

    const auto get_singleton =
        [](const TrackedDomain& domain) -> boost::optional<TrackedValue> {
      always_assert(domain.kind() == AbstractValueKind::Value);
      auto& elements = domain.elements();
      if (elements.size() != 1) {
        return boost::none;
      }
      return boost::optional<TrackedValue>(*elements.begin());
    };

    const auto escape_new_arrays = [&](uint32_t reg) {
      const auto& domain = current_state->get(reg);
      always_assert(domain.kind() == AbstractValueKind::Value);
      for (auto& value : domain.elements()) {
        if (is_new_array(value)) {
          if (is_array_literal(value)) {
            auto escaped_array = EscapedArrayDomain(get_aput_insns(value));
            auto it = m_escaped_arrays.find(value.new_array_insn);
            if (it == m_escaped_arrays.end()) {
              m_escaped_arrays.emplace(value.new_array_insn, escaped_array);
            } else {
              it->second.join_with(escaped_array);
            }
            TRACE(RAL, 4, "[RAL]   literal array escaped\n");
          } else {
            TRACE(RAL, 4, "[RAL]   non-literal array escaped\n");
            m_escaped_arrays[value.new_array_insn] = EscapedArrayDomain::top();
          }
        }
      }
    };

    const auto default_case = [&]() {
      // mark escaping arrays
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        escape_new_arrays(insn->src(i));
      }

      // If we get here, reset destination.
      if (insn->dests_size()) {
        set_current_state_at(insn->dest(), insn->dest_is_wide(),
                             TrackedDomain(make_other()));
      } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
        current_state->set(RESULT_REGISTER, TrackedDomain(make_other()));
      }
    };

    TRACE(RAL, 3, "[RAL] %s\n", SHOW(insn));
    switch (insn->opcode()) {
    case OPCODE_CONST:
      set_current_state_at(insn->dest(), false /* is_wide */,
                           TrackedDomain(make_literal(insn)));
      break;

    case OPCODE_NEW_ARRAY: {
      TRACE(RAL, 4, "[RAL]   new array of type %s\n", SHOW(insn->get_type()));
      const auto length = get_singleton(current_state->get(insn->src(0)));
      if (length && is_literal(*length)) {
        auto length_literal = get_literal(*length);
        TRACE(RAL, 4, "[RAL]     with length %ld\n", length_literal);
        always_assert(length_literal >= 0 && length_literal <= 2147483647);
        current_state->set(RESULT_REGISTER,
                           TrackedDomain(make_array(length_literal, insn)));
        break;
      }

      m_escaped_arrays[insn] = EscapedArrayDomain::top();
      default_case();
      break;
    }

    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
      const auto value = current_state->get(RESULT_REGISTER);
      set_current_state_at(insn->dest(), false /* is_wide */, value);
      break;
    }

    case OPCODE_APUT:
    case OPCODE_APUT_BYTE:
    case OPCODE_APUT_CHAR:
    case OPCODE_APUT_WIDE:
    case OPCODE_APUT_SHORT:
    case OPCODE_APUT_OBJECT:
    case OPCODE_APUT_BOOLEAN: {
      escape_new_arrays(insn->src(0));
      const auto array = get_singleton(current_state->get(insn->src(1)));
      const auto index = get_singleton(current_state->get(insn->src(2)));
      TRACE(RAL, 4, "[RAL]   aput: %d %d\n", array && is_new_array(*array),
            index && is_literal(*index));
      if (array && is_new_array(*array) && !is_array_literal(*array) && index &&
          is_literal(*index)) {
        int64_t index_literal = get_literal(*index);
        TRACE(RAL, 4, "[RAL]    index %ld of %u\n", index_literal,
              array->length);
        if (is_next_index(*array, index_literal)) {
          TRACE(RAL, 4, "[RAL]    is next\n");
          TrackedValue new_array = *array;
          if (add_element(new_array, index_literal, insn)) {
            current_state->set(insn->src(1), TrackedDomain(new_array));
            break;
          }
        }
      }

      default_case();
      break;
    }

    case OPCODE_MOVE: {
      const auto value = get_singleton(current_state->get(insn->src(0)));
      if (value && is_literal(*value)) {
        set_current_state_at(insn->dest(), false /* is_wide */,
                             TrackedDomain(*value));
        break;
      }

      default_case();
      break;
    }

    default: {
      default_case();
      break;
    }
    }
  }

  std::unordered_map<IRInstruction*, std::vector<IRInstruction*>>
  get_array_literals() {
    std::unordered_map<IRInstruction*, std::vector<IRInstruction*>> result;
    for (auto& p : m_escaped_arrays) {
      auto constant = p.second.get_constant();
      if (constant) {
        result.emplace(p.first, *constant);
      }
    }
    return result;
  }

 private:
  mutable std::unordered_map<IRInstruction*, EscapedArrayDomain>
      m_escaped_arrays;
};
} // namespace

////////////////////////////////////////////////////////////////////////////////

ReduceArrayLiterals::ReduceArrayLiterals(cfg::ControlFlowGraph& cfg) {
  bool any_new_array_insns = false;
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->opcode() == OPCODE_NEW_ARRAY) {
        any_new_array_insns = true;
        break;
      }
    }
  }

  if (any_new_array_insns) {
    Analyzer analyzer(cfg);
    m_array_literals = analyzer.get_array_literals();
  }
}

void ReduceArrayLiterals::patch(cfg::ControlFlowGraph& cfg,
                                size_t max_filled_elements,
                                int32_t min_sdk) {
  for (auto& p : m_array_literals) {
    IRInstruction* insn = p.first;
    std::vector<IRInstruction*>& aput_insns = p.second;
    if (aput_insns.size() == 0) {
      // Really no point of doing anything with these
      continue;
    }

    if (aput_insns.size() > max_filled_elements) {
      // TODO: Consider using an annotation-based scheme.
      // Or create various small arrays and then fuse them into a large one.
      m_stats.remaining_large_arrays++;
      m_stats.remaining_large_array_elements += aput_insns.size();
      continue;
    }

    auto type = insn->get_type();
    auto element_type = get_array_component_type(type);

    if (is_wide_type(element_type)) {
      // TODO: Consider using an annotation-based scheme.
      m_stats.remaining_wide_arrays++;
      m_stats.remaining_wide_array_elements += aput_insns.size();
      continue;
    }

    if (min_sdk < 19 && !is_primitive(element_type)) {
      // Before Kitkat, the Dalvik x86-atom backend had a bug for this case.
      // https://android.googlesource.com/platform/dalvik/+/ics-mr0/vm/mterp/out/InterpAsm-x86-atom.S#25106
      m_stats.remaining_buggy_arrays++;
      m_stats.remaining_buggy_array_elements += aput_insns.size();
      continue;
    }

    if (is_primitive(element_type) && element_type != get_int_type()) {
      // Somewhat surprising random implementation limitation in all known
      // ART versions:
      // https://android.googlesource.com/platform/art/+/400455c23d6a9a849d090b9e60ff53c4422e461b/runtime/interpreter/interpreter_common.cc#189
      m_stats.remaining_unimplemented_arrays++;
      m_stats.remaining_unimplemented_array_elements += aput_insns.size();
      continue;
    }

    m_stats.filled_arrays++;
    m_stats.filled_array_elements += aput_insns.size();

    // remove new-array instruction

    auto it = cfg.find_insn(insn);
    always_assert(insn->opcode() == OPCODE_NEW_ARRAY);
    auto move_result_it = cfg.move_result_of(it);
    always_assert(move_result_it->insn->opcode() ==
                  IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
    auto dest = move_result_it->insn->dest();
    cfg.remove_insn(it); // removes move-result-pseudo as well

    // insert filled-new-array instruction after the last aput

    IRInstruction* last_aput_insn = aput_insns.back();
    it = cfg.find_insn(last_aput_insn);

    std::vector<IRInstruction*> new_insns;
    IRInstruction* filled_new_array_insn =
        new IRInstruction(OPCODE_FILLED_NEW_ARRAY);
    filled_new_array_insn->set_type(type);
    filled_new_array_insn->set_arg_word_count(aput_insns.size());
    for (size_t index = 0; index < aput_insns.size(); index++) {
      filled_new_array_insn->set_src(index, cfg.allocate_temp());
    }
    new_insns.push_back(filled_new_array_insn);

    IRInstruction* move_result_object_insn =
        new IRInstruction(OPCODE_MOVE_RESULT_OBJECT);
    move_result_object_insn->set_dest(dest);
    new_insns.push_back(move_result_object_insn);

    cfg.insert_after(it, new_insns);

    // replace aput instructions with moves
    for (size_t index = 0; index < aput_insns.size(); index++) {
      IRInstruction* aput_insn = aput_insns[index];
      always_assert(is_aput(aput_insn->opcode()));
      always_assert(aput_insn->src(1) == dest);
      it = cfg.find_insn(aput_insn);
      IRInstruction* move_insn = new IRInstruction(
          is_primitive(element_type) ? OPCODE_MOVE : OPCODE_MOVE_OBJECT);
      move_insn->set_dest(filled_new_array_insn->src(index));
      move_insn->set_arg_word_count(1);
      move_insn->set_src(0, aput_insn->src(0));
      // TODO: Add a cfg::replace_insn method, instead of having to
      // insert_before followed by remove_insn
      cfg.insert_before(it, move_insn);
      cfg.remove_insn(it);
    }
  }
}

void ReduceArrayLiteralsPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles& /* conf */,
                                       PassManager& mgr) {
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  TRACE(RAL, 1, "[RAL] min_sdk=%d\n", mgr.get_redex_options().min_sdk);

  const auto scope = build_class_scope(stores);

  const auto stats = walk::parallel::reduce_methods<ReduceArrayLiterals::Stats>(
      scope,
      [&](DexMethod* m) {
        const auto code = m->get_code();
        if (code == nullptr) {
          return ReduceArrayLiterals::Stats();
        }

        code->build_cfg(/* editable */ true);
        ReduceArrayLiterals ral(code->cfg());
        ral.patch(code->cfg(), m_max_filled_elements, min_sdk);
        code->clear_cfg();
        return ral.get_stats();
      },
      [](ReduceArrayLiterals::Stats a, ReduceArrayLiterals::Stats b) {
        a.filled_arrays += b.filled_arrays;
        a.filled_array_elements += b.filled_array_elements;
        a.remaining_wide_arrays += b.remaining_wide_arrays;
        a.remaining_wide_array_elements += b.remaining_wide_array_elements;
        a.remaining_large_arrays += b.remaining_large_arrays;
        a.remaining_large_array_elements += b.remaining_large_array_elements;
        a.remaining_unimplemented_arrays += b.remaining_unimplemented_arrays;
        a.remaining_unimplemented_array_elements +=
            b.remaining_unimplemented_array_elements;
        a.remaining_buggy_arrays += b.remaining_buggy_arrays;
        a.remaining_buggy_array_elements += b.remaining_buggy_array_elements;
        return a;
      },
      ReduceArrayLiterals::Stats{},
      m_debug ? 1 : walk::parallel::default_num_threads());
  mgr.incr_metric(METRIC_FILLED_ARRAYS, stats.filled_arrays);
  mgr.incr_metric(METRIC_FILLED_ARRAY_ELEMENTS, stats.filled_array_elements);
  mgr.incr_metric(METRIC_REMAINING_WIDE_ARRAYS, stats.remaining_wide_arrays);
  mgr.incr_metric(METRIC_REMAINING_WIDE_ARRAY_ELEMENTS,
                  stats.remaining_wide_array_elements);
  mgr.incr_metric(METRIC_REMAINING_LARGE_ARRAYS, stats.remaining_large_arrays);
  mgr.incr_metric(METRIC_REMAINING_LARGE_ARRAY_ELEMENTS,
                  stats.remaining_large_array_elements);
  mgr.incr_metric(METRIC_REMAINING_UNIMPLEMENTED_ARRAYS,
                  stats.remaining_unimplemented_arrays);
  mgr.incr_metric(METRIC_REMAINING_UNIMPLEMENTED_ARRAY_ELEMENTS,
                  stats.remaining_unimplemented_array_elements);
  mgr.incr_metric(METRIC_REMAINING_BUGGY_ARRAYS, stats.remaining_buggy_arrays);
  mgr.incr_metric(METRIC_REMAINING_BUGGY_ARRAY_ELEMENTS,
                  stats.remaining_buggy_array_elements);
}

static ReduceArrayLiteralsPass s_pass;
