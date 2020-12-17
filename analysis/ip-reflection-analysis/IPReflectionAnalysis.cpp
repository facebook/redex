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

const std::string REFLECTION_ANALYSIS_RESULT_FILE =
    "redex-reflection-analysis.txt";

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

struct Summary : public AbstractDomain<Summary> {
  explicit Summary() : m_return(reflection::AbstractObjectDomain::bottom()) {}

  bool is_bottom() const override {
    return m_kind == sparta::AbstractValueKind::Bottom;
  }

  bool is_value() const { return m_kind == sparta::AbstractValueKind::Value; }

  bool is_top() const override {
    return m_kind == sparta::AbstractValueKind::Top;
  }

  bool leq(const Summary& other) const override {
    if (is_bottom()) {
      return true;
    } else if (m_kind == sparta::AbstractValueKind::Value) {
      return m_return.leq(other.m_return);
    } else {
      return other.is_top();
    }
  }

  bool equals(const Summary& other) const override {
    if (m_kind != other.m_kind) {
      return false;
    } else {
      return (m_kind == sparta::AbstractValueKind::Value)
                 ? m_return == other.m_return
                 : true;
    }
  }

  void set_to_bottom() override { not_reached(); }

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

  void join_with(const Summary& /* other */) override {
    throw std::runtime_error("join_with not implemented!");
  }

  void widen_with(const Summary& /* other */) override {
    throw std::runtime_error("widen_with not implemented!");
  }

  void meet_with(const Summary& /* other */) override {
    throw std::runtime_error("meet_with not implemented!");
  }

  void narrow_with(const Summary& /* other */) override {
    throw std::runtime_error("narrow_with not implemented!");
  }

 private:
  AbstractValueKind m_kind;
  reflection::AbstractObjectDomain m_return;
  reflection::ReflectionSites m_reflection_sites;
};

struct AnalysisParameters {
  // For speeding up reflection analysis
  reflection::MetadataCache refl_meta_cache;
};

using CallerContext = typename Caller::Domain;

template <typename Base>
class ReflectionAnalyzer : public Base {
 private:
  const DexMethod* m_method;
  Summary m_summary;

 public:
  explicit ReflectionAnalyzer(const DexMethod* method) : m_method(method) {}

  void analyze() override {
    if (!m_method) {
      return;
    }

    reflection::SummaryQueryFn query_fn =
        [&](const IRInstruction* insn) -> reflection::AbstractObjectDomain {
      auto callees = call_graph::resolve_callees_in_graph(
          *this->get_call_graph(), m_method, insn);

      reflection::AbstractObjectDomain ret =
          reflection::AbstractObjectDomain::bottom();
      for (const DexMethod* method : callees) {
        auto domain = this->get_summaries()
                          ->get(method, Summary::top())
                          .get_return_value();
        ret.join_with(domain);
      }
      return ret;
    };

    auto context = this->get_caller_context()->get(m_method);
    reflection::ReflectionAnalysis analysis(
        const_cast<DexMethod*>(m_method),
        &context,
        &query_fn,
        &this->get_analysis_parameters()->refl_meta_cache);

    m_summary.set_value(analysis.get_return_value());
    m_summary.set_reflection_sites(analysis.get_reflection_sites());

    auto partition = analysis.get_calling_context_partition();
    if (!partition.is_top() && !partition.is_bottom()) {
      for (const auto& entry : partition.bindings()) {
        auto insn = entry.first;
        auto calling_context = entry.second;
        auto op = insn->opcode();
        always_assert(is_invoke(op));

        auto callees = call_graph::resolve_callees_in_graph(
            *this->get_call_graph(), m_method, insn);

        for (const DexMethod* method : callees) {
          this->get_caller_context()->update(
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
    this->get_summaries()->maybe_update(m_method, [&](Summary& old) {
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
  using Registry = MethodSummaryRegistry<Summary>;
  using FunctionSummary = Summary;

  template <typename IntraproceduralBase>
  using FunctionAnalyzer = ReflectionAnalyzer<IntraproceduralBase>;

  template <typename GraphInterface, typename Domain>
  using FixpointIteratorBase =
      sparta::ParallelMonotonicFixpointIterator<GraphInterface, Domain>;

  using Callsite = Caller;
};

using Analysis =
    InterproceduralAnalyzer<ReflectionAnalysisAdaptor, AnalysisParameters>;

} // namespace

void IPReflectionAnalysisPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& /* pm */) {

  Scope scope = build_class_scope(stores);
  AnalysisParameters param;
  auto analysis = Analysis(scope, m_max_iteration, &param);
  analysis.run();
  auto summaries = analysis.registry.get_map();
  m_result = std::make_shared<Result>();
  for (const auto& entry : summaries) {
    (*m_result)[entry.first] = entry.second.get_reflection_sites();
  }
  if (m_export_results) {
    std::string results_filename =
        conf.metafile(REFLECTION_ANALYSIS_RESULT_FILE);
    std::ofstream file(results_filename);

    for (const auto& entry : *m_result) {
      if (!entry.second.empty()) {
        file << show(entry.first) << " -> " << entry.second << std::endl;
      }
    }
  }
}

static IPReflectionAnalysisPass s_pass;
