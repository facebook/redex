/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SideEffectSummary.h"

#include "CallGraph.h"
#include "ConcurrentContainers.h"
#include "Walkers.h"

using namespace sparta;

namespace ptrs = local_pointers;

namespace {

using reg_t = uint32_t;

using ParamInstructionMap =
    std::unordered_map<const IRInstruction*, param_idx_t>;

using PointersFixpointIteratorMap =
    ConcurrentMap<const DexMethodRef*, ptrs::FixpointIterator*>;

class EffectSummaryBuilder final {
 public:
  EffectSummaryBuilder(
      const EffectSummaryMap& effect_summaries,
      const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
      const ptrs::FixpointIterator& ptrs_fp_iter,
      MethodRefCache* mref_cache,
      const IRCode* code)
      : m_effect_summaries(effect_summaries),
        m_non_overridden_virtuals(non_overridden_virtuals),
        m_ptrs_fp_iter(ptrs_fp_iter),
        m_mref_cache(mref_cache),
        m_code(code) {
    auto idx = 0;
    for (auto& mie : InstructionIterable(code->get_param_instructions())) {
      auto insn = mie.insn;
      m_param_insn_map.emplace(insn, idx++);
    }
  }

  EffectSummary build() {
    EffectSummary summary;

    // Aggregate the effects of each individual instruction in the code object.
    auto& cfg = m_code->cfg();
    for (auto* block : cfg.blocks()) {
      auto env = m_ptrs_fp_iter.get_entry_state_at(block);
      if (env.is_bottom()) {
        continue;
      }
      for (auto& mie : InstructionIterable(block)) {
        auto* insn = mie.insn;
        analyze_instruction_effects(env, insn, &summary);
        m_ptrs_fp_iter.analyze_instruction(insn, &env);
      }
    }

    return summary;
  }

 private:
  void analyze_instruction_effects(const ptrs::Environment& env,
                                   const IRInstruction* insn,
                                   EffectSummary* summary) {
    auto op = insn->opcode();
    switch (op) {
    case OPCODE_THROW: {
      summary->effects |= EFF_THROWS;
      break;
    }
    case OPCODE_MONITOR_ENTER:
    case OPCODE_MONITOR_EXIT: {
      summary->effects |= EFF_LOCKS;
      break;
    }
    case OPCODE_SPUT:
    case OPCODE_SPUT_WIDE:
    case OPCODE_SPUT_BOOLEAN:
    case OPCODE_SPUT_BYTE:
    case OPCODE_SPUT_CHAR:
    case OPCODE_SPUT_SHORT:
    case OPCODE_SPUT_OBJECT: {
      summary->effects |= EFF_WRITE_MAY_ESCAPE;
      break;
    }
    case OPCODE_IPUT:
    case OPCODE_IPUT_WIDE:
    case OPCODE_IPUT_BOOLEAN:
    case OPCODE_IPUT_BYTE:
    case OPCODE_IPUT_CHAR:
    case OPCODE_IPUT_SHORT:
    case OPCODE_IPUT_OBJECT:

    case OPCODE_APUT:
    case OPCODE_APUT_WIDE:
    case OPCODE_APUT_BOOLEAN:
    case OPCODE_APUT_BYTE:
    case OPCODE_APUT_CHAR:
    case OPCODE_APUT_SHORT:
    case OPCODE_APUT_OBJECT: {
      classify_heap_write(env, insn->src(1), summary);
      break;
    }

    case OPCODE_FILL_ARRAY_DATA: {
      classify_heap_write(env, insn->src(0), summary);
      break;
    }

    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_INTERFACE: {
      TRACE(DEAD_CODE, 3, "Unknown invoke: %s\n", SHOW(insn));
      summary->effects |= EFF_UNKNOWN_INVOKE;
      break;
    }
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL: {
      TRACE(DEAD_CODE, 3, "Unknown invoke: %s\n", SHOW(insn));
      auto method = resolve_method(insn->get_method(), opcode_to_search(insn),
                                   *m_mref_cache);
      if (op == OPCODE_INVOKE_VIRTUAL &&
          !m_non_overridden_virtuals.count(method)) {
        summary->effects |= EFF_UNKNOWN_INVOKE;
        break;
      }
      auto summ_it = m_effect_summaries.find(method);
      if (summ_it == m_effect_summaries.end()) {
        TRACE(DEAD_CODE, 3, "Unknown invoke: %s\n", SHOW(insn));
        summary->effects |= EFF_UNKNOWN_INVOKE;
        break;
      }
      auto& callee_summary = summ_it->second;
      summary->effects |= callee_summary.effects;
      for (auto idx : callee_summary.modified_params) {
        classify_heap_write(env, insn->src(idx), summary);
      }
      break;
    }
    default: { break; }
    }
  }

  /*
   * Given a write to the heap, classify it as one of the following:
   *   - Write to a locally-allocated non-escaping object
   *   - Write to an object passed in as a parameter
   *   - Write to an escaping and/or unknown object
   */
  void classify_heap_write(const ptrs::Environment& env,
                           reg_t modified_ptr_reg,
                           EffectSummary* summary) {
    auto pointers = env.get_pointers(modified_ptr_reg);
    if (!pointers.is_value()) {
      summary->effects |= EFF_WRITE_MAY_ESCAPE;
      return;
    }
    for (auto insn : pointers.elements()) {
      if (opcode::is_load_param(insn->opcode())) {
        summary->modified_params.emplace(m_param_insn_map.at(insn));
      } else if (env.get_pointee(insn).equals(
                     EscapeDomain(EscapeState::MAY_ESCAPE))) {
        TRACE(DEAD_CODE, 3, "Escaping write to value allocated by %s\n",
              SHOW(insn));
        summary->effects |= EFF_WRITE_MAY_ESCAPE;
      }
    }
  }

  // Map of load-param instruction -> parameter index
  ParamInstructionMap m_param_insn_map;
  const EffectSummaryMap& m_effect_summaries;
  const std::unordered_set<const DexMethod*>& m_non_overridden_virtuals;
  const ptrs::FixpointIterator& m_ptrs_fp_iter;
  MethodRefCache* m_mref_cache;
  const IRCode* m_code;
};

} // namespace

EffectSummary analyze_code_effects(
    const EffectSummaryMap& effect_summaries,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    const ptrs::FixpointIterator& ptrs_fp_iter,
    MethodRefCache* mref_cache,
    const IRCode* code) {
  return EffectSummaryBuilder(effect_summaries, non_overridden_virtuals,
                              ptrs_fp_iter, mref_cache, code)
      .build();
}

// TODO: Write a generic version of this algorithm, it seems useful in a number
// of places.
std::vector<DexMethod*> reverse_tsort_by_calls(
    const Scope& scope,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals) {
  std::vector<DexMethod*> result;
  std::unordered_set<const DexMethod*> visiting;
  std::unordered_set<const DexMethod*> visited;
  MethodRefCache mref_cache;
  std::function<void(DexMethod*)> visit = [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }
    if (visited.count(method)) {
      return;
    }
    if (visiting.count(method)) {
      return;
    }
    visiting.emplace(method);
    for (auto& mie : InstructionIterable(method->get_code())) {
      auto insn = mie.insn;
      auto op = insn->opcode();
      if (op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_STATIC ||
          op == OPCODE_INVOKE_VIRTUAL) {
        auto callee = resolve_method(insn->get_method(), opcode_to_search(insn),
                                     mref_cache);
        if (op == OPCODE_INVOKE_VIRTUAL &&
            !non_overridden_virtuals.count(callee)) {
          continue;
        }
        if (callee != nullptr) {
          visit(callee);
        }
      }
    }
    visiting.erase(method);
    result.emplace_back(method);
    visited.emplace(method);
  };
  walk::code(scope, [&](DexMethod* method, IRCode&) { visit(method); });
  return result;
}

void analyze_methods(
    const Scope& scope,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    PointersFixpointIteratorMap& ptrs_fp_iter_map,
    MethodRefCache* mref_cache,
    EffectSummaryMap* effect_summaries) {
  // We get better analysis results if we know the summaries of the callees, so
  // we analyze the methods in reverse topological order.
  for (auto* method : reverse_tsort_by_calls(scope, non_overridden_virtuals)) {
    if (effect_summaries->count(method) != 0) {
      continue;
    }
    if (!method->get_code()) {
      (*effect_summaries)[method].effects |= EFF_UNKNOWN_INVOKE;
      continue;
    }
    const auto& ptrs_fp_iter = ptrs_fp_iter_map.find(method)->second;
    (*effect_summaries)[method] =
        analyze_code_effects(*effect_summaries, non_overridden_virtuals,
                             *ptrs_fp_iter, mref_cache, method->get_code());

    if (traceEnabled(DEAD_CODE, 3)) {
      const auto& summary = effect_summaries->at(method);
      TRACE(DEAD_CODE, 3, "%s %s unknown side effects (%u)\n", SHOW(method),
            summary.effects != EFF_NONE ? "has" : "does not have",
            summary.effects);
      if (summary.modified_params.size() != 0) {
        TRACE(DEAD_CODE, 3, "Modified params: ");
        for (auto idx : summary.modified_params) {
          TRACE(DEAD_CODE, 3, "%u ", idx);
        }
        TRACE(DEAD_CODE, 3, "\n");
      }
    }
  }
}

void summarize_all_method_effects(
    const Scope& scope,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    EffectSummaryMap* effect_summaries) {
  // This method is special: the bytecode verifier requires that this method
  // be called before a newly-allocated object gets used in any way. We can
  // model this by treating the method as modifying its `this` parameter --
  // changing it from uninitialized to initialized.
  (*effect_summaries)[DexMethod::get_method("Ljava/lang/Object;.<init>:()V")] =
      EffectSummary({0});

  PointersFixpointIteratorMap ptrs_fp_iter_map;
  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    auto fp_iter = new ptrs::FixpointIterator(code.cfg());
    fp_iter->run(ptrs::Environment());
    ptrs_fp_iter_map.insert(std::make_pair(method, fp_iter));
  });
  MethodRefCache mref_cache;
  // TODO: This call iterates serially over all the methods; it's the biggest
  // bottleneck of the pass & should be parallelized.
  analyze_methods(scope, non_overridden_virtuals, ptrs_fp_iter_map, &mref_cache,
                  effect_summaries);
  for (auto& pair : ptrs_fp_iter_map) {
    delete pair.second;
  }
}

s_expr EffectSummary::to_s_expr() const {
  std::vector<s_expr> s_exprs;
  s_exprs.emplace_back(std::to_string(effects));
  std::vector<s_expr> mod_param_s_exprs;
  for (auto idx : modified_params) {
    mod_param_s_exprs.emplace_back(idx);
  }
  s_exprs.emplace_back(mod_param_s_exprs);
  return s_expr(s_exprs);
}

boost::optional<EffectSummary> EffectSummary::from_s_expr(const s_expr& expr) {
  if (expr.size() != 2) {
    return boost::none;
  }
  EffectSummary summary;
  if (!expr[0].is_string()) {
    return boost::none;
  }
  summary.effects = std::stoi(expr[0].str());
  if (!expr[1].is_list()) {
    return boost::none;
  }
  for (size_t i = 0; i < expr[1].size(); ++i) {
    summary.modified_params.emplace(expr[1][i].get_int32());
  }
  return summary;
}

void load_effect_summaries(const std::string& filename,
                           EffectSummaryMap* effect_summaries) {
  std::ifstream file_input(filename);
  s_expr_istream s_expr_input(file_input);
  size_t load_count{0};
  while (s_expr_input.good()) {
    s_expr expr;
    s_expr_input >> expr;
    if (s_expr_input.eoi()) {
      break;
    }
    always_assert_log(!s_expr_input.fail(), "%s\n",
                      s_expr_input.what().c_str());
    DexMethodRef* dex_method =
        DexMethod::get_method(expr[0].get_string().c_str());
    if (dex_method == nullptr) {
      continue;
    }
    auto summary_opt = EffectSummary::from_s_expr(expr[1]);
    always_assert_log(summary_opt, "Couldn't parse S-expression: %s\n",
                      expr.str().c_str());
    auto it = effect_summaries->find(dex_method);
    if (it == effect_summaries->end()) {
      effect_summaries->emplace(dex_method, *summary_opt);
    } else {
      TRACE(DEAD_CODE, 2, "Collision with summary for method %s\n",
            SHOW(dex_method));
    }
    ++load_count;
  }
  TRACE(DEAD_CODE, 2, "Loaded %lu summaries from %s\n", load_count,
        filename.c_str());
}
