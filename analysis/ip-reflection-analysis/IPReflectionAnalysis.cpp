/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IPReflectionAnalysis.h"

#include "AbstractDomain.h"
#include "CallGraph.h"
#include "MethodOverrideGraph.h"
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

  Domain analyze_edge(const std::shared_ptr<call_graph::Edge>& edge,
                      const Domain& original) {
    auto callee = edge->callee()->method();
    if (!callee) {
      return Domain::bottom();
    }

    Domain retval;
    retval.update(callee, [&](const reflection::CallingContext&) {
      return original.get(callee);
    });
    return retval;
  }
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

  reflection::AbstractObjectDomain get_return_value() {
    if (is_top()) {
      return reflection::AbstractObjectDomain::top();
    }
    if (is_bottom()) {
      return reflection::AbstractObjectDomain::bottom();
    }
    return m_return;
  }

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

namespace mog = method_override_graph;

struct Metadata {
  std::unique_ptr<const mog::Graph> method_override_graph;
};

template <typename FunctionSummaries>
class ReflectionAnalyzer : public Intraprocedural {
 private:
  using CallerContext = typename Caller::Domain;
  const DexMethod* m_method;
  FunctionSummaries* m_summaries;
  CallerContext* m_context;
  FunctionSummary m_summary;
  Metadata* m_metadata;

 public:
  ReflectionAnalyzer(const DexMethod* method,
                     FunctionSummaries* summaries,
                     CallerContext* context,
                     Metadata* metadata)
      : m_method(method),
        m_summaries(summaries),
        m_context(context),
        m_metadata(metadata) {}

  void analyze() override {
    if (!m_method) {
      return;
    }

    reflection::SummaryQueryFn query_fn =
        [&](const DexMethod* callee) -> reflection::AbstractObjectDomain {
      auto ret =
          m_summaries->get(callee, FunctionSummary::top()).get_return_value();

      std::unordered_set<const DexMethod*> overriding_methods =
          mog::get_overriding_methods(*(m_metadata->method_override_graph),
                                      callee);

      for (const DexMethod* method : overriding_methods) {
        ret.join_with(m_summaries->get(method, FunctionSummary::top())
                          .get_return_value());
      }
      return ret;
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

        m_context->update(
            callee, [&](const reflection::CallingContext& original_context) {
              return calling_context.join(original_context);
            });

        std::unordered_set<const DexMethod*> overriding_methods;
        if (is_invoke_virtual(op)) {
          overriding_methods = mog::get_overriding_methods(
              *(m_metadata->method_override_graph), callee);
        } else if (is_invoke_interface(op)) {
          overriding_methods = mog::get_overriding_methods(
              *(m_metadata->method_override_graph), callee, true);
        }

        for (const DexMethod* method : overriding_methods) {
          m_context->update(
              method, [&](const reflection::CallingContext& original_context) {
                return calling_context.join(original_context);
              });
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

  template <typename GraphInterface, typename Domain>
  using FixpointIteratorBase =
      sparta::ParallelMonotonicFixpointIterator<GraphInterface, Domain>;

  using Callsite = Caller;
};

using Analysis = InterproceduralAnalyzer<ReflectionAnalysisAdaptor, Metadata>;

} // namespace

void IPReflectionAnalysisPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* conf */,
                                        PassManager& /* pm */) {

  Scope scope = build_class_scope(stores);
  Metadata metadata;
  metadata.method_override_graph = mog::build_graph(scope);
  auto analysis = Analysis(scope, m_max_iteration, &metadata);
  analysis.run();
  auto summaries = analysis.registry.get_map();
  m_result = std::make_shared<Result>();
  for (const auto& entry : summaries) {
    (*m_result)[entry.first] = entry.second.get_reflection_sites();
  }
}

static IPReflectionAnalysisPass s_pass;
