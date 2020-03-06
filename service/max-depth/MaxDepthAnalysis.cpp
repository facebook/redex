/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MaxDepthAnalysis.h"

#include "AbstractDomain.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "HashedSetAbstractDomain.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "SpartaInterprocedural.h"

namespace {

using namespace sparta;
using namespace sparta_interprocedural;

// Description for the analysis

// - We initialize the max depth of each method to Top, which means unknown or
//   potentially infinite depth of calls.
// - Every step will progressively reduce the depth by considering the cases
//   where the depth is known and is not Top.
// - The steps are iterated until a global fixpoint for the summaries is found.

// Define an abstract domain as a summary for a method. The summary should
// contain what properties we are interested in knowing such a method.
struct DepthDomain : public AbstractDomain<DepthDomain> {
  explicit DepthDomain() : m_depth(0), m_kind(sparta::AbstractValueKind::Top) {}

  explicit DepthDomain(unsigned i)
      : m_depth(i), m_kind(sparta::AbstractValueKind::Value) {}

  bool is_bottom() const override {
    return m_kind == sparta::AbstractValueKind::Bottom;
  }

  bool is_value() const { return m_kind == sparta::AbstractValueKind::Value; }

  bool is_top() const override {
    return m_kind == sparta::AbstractValueKind::Top;
  }

  bool leq(const DepthDomain& other) const override {
    if (is_bottom()) {
      return true;
    } else if (m_kind == sparta::AbstractValueKind::Value) {
      return m_depth <= other.m_depth;
    } else {
      return other.is_top();
    }
  }

  bool equals(const DepthDomain& other) const override {
    if (m_kind != other.m_kind) {
      return false;
    } else {
      return (m_kind == sparta::AbstractValueKind::Value)
                 ? m_depth == other.m_depth
                 : true;
    }
  }

  void set_to_bottom() override { always_assert(false); }
  virtual void set_value(unsigned depth) {
    m_kind = sparta::AbstractValueKind::Value;
    m_depth = depth;
  }
  void set_to_top() override { m_kind = sparta::AbstractValueKind::Top; }

  void join_with(const DepthDomain& other) override {
    if (is_bottom() || other.is_top()) {
      *this = other;
    } else if (is_value() && other.is_value()) {
      if (other.m_depth > m_depth) {
        m_depth = other.m_depth;
      }
    }
  }

  void widen_with(const DepthDomain& other) override { join_with(other); }

  void meet_with(const DepthDomain& /* other */) override {
    throw std::runtime_error("meet_with not implemented!");
  }

  void narrow_with(const DepthDomain& /* other */) override {
    throw std::runtime_error("narrow_with not implemented!");
  }

  unsigned depth() const { return m_depth; }

 private:
  unsigned m_depth;
  AbstractValueKind m_kind;
};

// Callsite is mostly used to describe calling context. It can be partitioned
// based on call edges. In this analysis, the call depth is irrelevant to the
// calling context, so we leave it unused.
struct Caller {
  using Domain = HashedSetAbstractDomain<DexMethod*>;
};

// Core part of the analysis. This analyzer should be similar to an
// intraprocedural analysis, except that we have access to the summaries and the
// calling context.
template <typename FunctionSummaries>
class MaxDepthFunctionAnalyzer : public Intraprocedural {
 private:
  using CallerContext = typename Caller::Domain;
  const DexMethod* m_method;
  FunctionSummaries* m_summaries;
  CallerContext* m_context;
  DepthDomain m_domain;

 public:
  MaxDepthFunctionAnalyzer(const DexMethod* method,
                           FunctionSummaries* summaries,
                           CallerContext* context)
      : m_method(method),
        m_summaries(summaries),
        m_context(context),
        m_domain(0) {}

  void analyze() override {
    if (!m_method) {
      return;
    }
    auto code = m_method->get_code();
    if (!code) {
      return;
    }
    for (auto& mie : InstructionIterable(code)) {
      always_assert_log(mie.insn,
                        "IR is malformed, MIE holding an nullptr instruction.");

      analyze_insn(mie.insn);
    }
  }

  void analyze_insn(IRInstruction* insn) {
    if (is_invoke(insn->opcode())) {
      analyze_invoke(insn);
    }
  }

  void analyze_invoke(IRInstruction* insn) {
    auto callee = insn->get_method();
    auto callee_method = resolve_method(callee, opcode_to_search(insn));
    if (callee_method) {
      auto summary = m_summaries->get(callee_method);
      if (summary.is_value()) {
        m_domain.join_with(DepthDomain(summary.depth() + 1u));
      } else {
        m_domain.join_with(summary);
      }
    } else {
      m_domain.join_with(DepthDomain(1u));
    }
  }

  void summarize() override {
    if (!m_method) {
      return;
    }
    m_summaries->update(m_method, [&](const DepthDomain&) { return m_domain; });
  }
};

// The adaptor supplies the necessary typenames to the analyzer so that template
// instantiation assembles the different parts. It's also possible to override
// type aliases in the adaptor base class.
struct MaxDepthAnalysisAdaptor : public BottomUpAnalysisAdaptorBase {
  // This map type is used to hold the summaries
  template <typename K, typename V>
  using Map = PatriciaTreeMapAbstractEnvironment<K, V>;
  using FunctionSummary = DepthDomain;

  template <typename Summaries>
  using FunctionAnalyzer = MaxDepthFunctionAnalyzer<Summaries>;
  using Callsite = Caller;
};

using Analysis = InterproceduralAnalyzer<MaxDepthAnalysisAdaptor>;

} // namespace

namespace max_depth {

std::unordered_map<const DexMethod*, int> analyze(const Scope& scope,
                                                  unsigned max_iteration) {
  auto analysis = Analysis(scope, max_iteration);
  analysis.run();
  std::unordered_map<const DexMethod*, int> results;
  if (analysis.function_summaries.is_top()) {
    // nothing is in there.
    return results;
  }
  for (const auto& entry : analysis.function_summaries.bindings()) {
    if (entry.second.is_value()) {
      results[entry.first] = entry.second.depth();
    }
  }
  return results;
}

} // namespace max_depth
