/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IPReflectionAnalysis.h"

#include "AbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeMapAbstractPartition.h"
#include "Resolver.h"
#include "SpartaInterprocedural.h"

namespace {

using namespace sparta;
using namespace sparta_interprocedural;

struct Caller {
  // A map from callee to its calling context.
  using Domain = PatriciaTreeMapAbstractPartition<const DexMethod*,
                                                  reflection::CallingContext>;
};

struct FunctionSummary : public AbstractDomain<FunctionSummary> {
  explicit FunctionSummary()
      : m_return(reflection::AbstractObjectDomain::bottom()) {}

  bool is_bottom() const override {
    return m_kind == sparta::AbstractValueKind::Bottom;
  }

  bool is_value() const { return m_kind == sparta::AbstractValueKind::Value; }

  bool is_top() const override {
    return m_kind == sparta::AbstractValueKind::Top;
  }

  bool leq(const FunctionSummary& other) const override {
    if (is_bottom()) {
      return true;
    } else if (m_kind == sparta::AbstractValueKind::Value) {
      return m_return.leq(other.m_return);
    } else {
      return other.is_top();
    }
  }

  bool equals(const FunctionSummary& other) const override {
    if (m_kind != other.m_kind) {
      return false;
    } else {
      return (m_kind == sparta::AbstractValueKind::Value)
                 ? m_return == other.m_return
                 : true;
    }
  }

  void set_to_bottom() override { always_assert(false); }

  void set_value(reflection::AbstractObjectDomain retval) {
    m_kind = sparta::AbstractValueKind::Value;
    m_return = std::move(retval);
  }

  reflection::AbstractObjectDomain get_return_value() { return m_return; }

  void set_reflection_sites(reflection::ReflectionSites sites) {
    m_reflection_sites = std::move(sites);
  }

  reflection::ReflectionSites get_reflection_sites() const {
    return m_reflection_sites;
  }

  void set_to_top() override { m_kind = sparta::AbstractValueKind::Top; }

  void join_with(const FunctionSummary& /* other */) override {
    throw std::runtime_error("join_with not implemented!");
  }

  void widen_with(const FunctionSummary& /* other */) override {
    throw std::runtime_error("widen_with not implemented!");
  }

  void meet_with(const FunctionSummary& /* other */) override {
    throw std::runtime_error("meet_with not implemented!");
  }

  void narrow_with(const FunctionSummary& /* other */) override {
    throw std::runtime_error("narrow_with not implemented!");
  }

 private:
  AbstractValueKind m_kind;
  reflection::AbstractObjectDomain m_return;
  reflection::ReflectionSites m_reflection_sites;
};

template <typename FunctionSummaries>
class ReflectionAnalyzer : public Intraprocedural {
 private:
  using CallerContext = typename Caller::Domain;
  const DexMethod* m_method;
  FunctionSummaries* m_summaries;
  CallerContext* m_context;
  FunctionSummary m_summary;

 public:
  ReflectionAnalyzer(const DexMethod* method,
                     FunctionSummaries* summaries,
                     CallerContext* context,
                     void* /* metadata */)
      : m_method(method), m_summaries(summaries), m_context(context) {}

  void analyze() override {
    if (!m_method) {
      return;
    }

    reflection::SummaryQueryFn query_fn =
        [&](const DexMethod* callee) -> reflection::AbstractObjectDomain {
      auto summary = m_summaries->get(callee, FunctionSummary::top());
      if (summary.is_value()) {
        return summary.get_return_value();
      } else if (summary.is_top()) {
        return reflection::AbstractObjectDomain::top();
      } else {
        return reflection::AbstractObjectDomain::bottom();
      }
    };

    auto context = m_context->get(m_method);
    reflection::ReflectionAnalysis analysis(const_cast<DexMethod*>(m_method),
                                            &context, &query_fn);

    m_summary.set_value(analysis.get_return_value());
    m_summary.set_reflection_sites(std::move(analysis.get_reflection_sites()));

    auto partition = analysis.get_calling_context_partition();
    if (!partition.is_top() && !partition.is_bottom()) {
      for (const auto& entry : partition.bindings()) {
        auto insn = entry.first;
        auto calling_context = entry.second;
        auto op = insn->opcode();
        always_assert(is_invoke(op));
        DexMethod* callee =
            resolve_method(insn->get_method(), opcode_to_search(op));
        if (is_invoke_static(op) || is_invoke_direct(op) ||
            is_invoke_super(op)) {
          m_context->update(
              callee, [&](const reflection::CallingContext& original_context) {
                return calling_context.join(original_context);
              });
        } else if (is_invoke_virtual(op) || is_invoke_interface(op)) {
          // TODO: Handle virtual calls
        }
      }
    }
  }

  void summarize() override {
    if (!m_method) {
      return;
    }
    m_summaries->maybe_update(m_method, [&](FunctionSummary& old) {
      if (old == m_summary) {
        // no change will be made
        return false;
      }
      old = m_summary; // overwrite previous value
      return true;
    });
  }
};

struct ReflectionAnalysisAdaptor : public AnalysisAdaptorBase {
  using Registry = MethodSummaryRegistry<FunctionSummary>;
  using FunctionSummary = FunctionSummary;
  using FunctionAnalyzer = ReflectionAnalyzer<Registry>;
  using Callsite = Caller;
};

using Analysis = InterproceduralAnalyzer<ReflectionAnalysisAdaptor>;

} // namespace

void IPReflectionAnalysisPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* conf */,
                                        PassManager& /* pm */) {
  auto analysis = Analysis(build_class_scope(stores), m_max_iteration);
  analysis.run();
  auto summaries = analysis.registry.get_map();
  m_result = std::make_shared<Result>();
  for (const auto& entry : summaries) {
    (*m_result)[entry.first] = entry.second.get_reflection_sites();
  }
}

static IPReflectionAnalysisPass s_pass;
