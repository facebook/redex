/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IPConstantPropagation.h"

#include "ConfigFiles.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationState.h"
#include "ConstructorParams.h"
#include "DefinitelyAssignedIFields.h"
#include "IPConstantPropagationAnalysis.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Purity.h"
#include "RedexResources.h"
#include "ScopedMetrics.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"
#include "WrappedPrimitives.h"

namespace mog = method_override_graph;

namespace {

// Whether, for a given type, a non-top value represents useful information.
bool is_useful(DexType* type, const ConstantValue& value) {
  always_assert(!value.is_top());
  if (type::is_boolean(type)) {
    // Since a boolean value can only have 1 and 0 as values, "GEZ" tells us
    // nothing useful.
    return !value.equals(SignedConstantDomain(sign_domain::Interval::GEZ)) &&
           !value.equals(SignedConstantDomain(0, 1));
  }
  return true;
}

} // namespace

namespace constant_propagation {

namespace interprocedural {

using CombinedAnalyzer =
    InstructionAnalyzerCombiner<ClinitFieldAnalyzer,
                                ImmutableAttributeAnalyzer,
                                WholeProgramAwareAnalyzer,
                                EnumFieldAnalyzer,
                                BoxedBooleanAnalyzer,
                                StringAnalyzer,
                                ConstantClassObjectAnalyzer,
                                ApiLevelAnalyzer,
                                PackageNameAnalyzer,
                                NewObjectAnalyzer,
                                PrimitiveAnalyzer>;

class AnalyzerGenerator {
  const ImmutableAttributeAnalyzerState* m_immut_analyzer_state;
  const ApiLevelAnalyzerState* m_api_level_analyzer_state;
  const StringAnalyzerState* m_string_analyzer_state;
  const PackageNameState* m_package_name_state;
  const State& m_cp_state;

 public:
  explicit AnalyzerGenerator(
      const ImmutableAttributeAnalyzerState* immut_analyzer_state,
      const ApiLevelAnalyzerState* api_level_analyzer_state,
      const StringAnalyzerState* string_analyzer_state,
      const PackageNameState* package_name_state,
      const State& cp_state)
      : m_immut_analyzer_state(immut_analyzer_state),
        m_api_level_analyzer_state(api_level_analyzer_state),
        m_string_analyzer_state(string_analyzer_state),
        m_package_name_state(package_name_state),
        m_cp_state(cp_state) {
    // Initialize the singletons that `operator()` needs ahead of time to
    // avoid a data race.
    static_cast<void>(EnumFieldAnalyzerState::get());
    static_cast<void>(BoxedBooleanAnalyzerState::get());
    static_cast<void>(ApiLevelAnalyzerState::get());
  }

  std::unique_ptr<IntraproceduralAnalysis> operator()(
      const DexMethod* method,
      const WholeProgramState& wps,
      ArgumentDomain args) {
    always_assert(method->get_code() != nullptr);
    auto& code = *method->get_code();
    // Currently, our callgraph does not include calls to non-devirtualizable
    // virtual methods. So those methods may appear unreachable despite being
    // reachable.
    if (args.is_bottom()) {
      args.set_to_top();
    } else if (!args.is_top()) {
      TRACE(ICONSTP, 3, "Have args for %s: %s", SHOW(method), SHOW(args));
    }

    auto env = env_with_params(is_static(method), &code, args);
    DexType* class_under_init{nullptr};
    if (method::is_clinit(method)) {
      class_under_init = method->get_class();
      set_encoded_values(type_class(class_under_init), &env);
    }
    TRACE(ICONSTP, 5, "%s", SHOW(code.cfg()));

    auto wps_accessor = std::make_unique<WholeProgramStateAccessor>(wps);
    auto wps_accessor_ptr = wps_accessor.get();
    auto immut_analyzer_state =
        const_cast<ImmutableAttributeAnalyzerState*>(m_immut_analyzer_state);
    return std::make_unique<IntraproceduralAnalysis>(
        &m_cp_state, std::move(wps_accessor), code.cfg(),
        CombinedAnalyzer(
            class_under_init, immut_analyzer_state, wps_accessor_ptr,
            EnumFieldAnalyzerState::get(), BoxedBooleanAnalyzerState::get(),
            const_cast<StringAnalyzerState*>(m_string_analyzer_state), nullptr,
            *const_cast<ApiLevelAnalyzerState*>(m_api_level_analyzer_state),
            const_cast<PackageNameState*>(m_package_name_state),
            immut_analyzer_state, nullptr),
        std::move(env));
  }
};

/*
 * This algorithm is based off the approach in this paper[1]. We start off by
 * assuming no knowledge of any field values or method return values, i.e. we
 * just interprocedurally propagate constants from const opcodes. Then, we use
 * the result of that "bootstrap" run to build an approximation of the field
 * and method return values, which is represented by a WholeProgramState. We
 * re-run propagation using that WholeProgramState until we reach a fixpoint or
 * a configurable limit.
 *
 * [1]: Venet, Arnaud. Precise and Efficient Static Array Bound Checking for
 *      Large Embedded C Programs.
 *      https://ntrs.nasa.gov/search.jsp?R=20040081118
 */
std::unique_ptr<FixpointIterator> PassImpl::analyze(
    const Scope& scope,
    const ImmutableAttributeAnalyzerState* immut_analyzer_state,
    const ApiLevelAnalyzerState* api_level_analyzer_state,
    const StringAnalyzerState* string_analyzer_state,
    const PackageNameState* package_name_state,
    const State& cp_state) {
  auto method_override_graph = mog::build_graph(scope);
  std::shared_ptr<call_graph::Graph> cg;
  {
    cg = m_config.use_multiple_callee_callgraph
             ? std::make_shared<call_graph::Graph>(
                   call_graph::multiple_callee_graph(
                       *method_override_graph, scope,
                       m_config.big_override_threshold))
             : std::make_shared<call_graph::Graph>(
                   call_graph::single_callee_graph(*method_override_graph,
                                                   scope));
  }
  auto cg_for_wps = m_config.use_multiple_callee_callgraph ? cg : nullptr;
  auto cg_stats = get_num_nodes_edges(*cg);
  m_stats.callgraph_nodes = cg_stats.num_nodes;
  m_stats.callgraph_edges = cg_stats.num_edges;
  m_stats.callgraph_callsites = cg_stats.num_callsites;
  auto fp_iter = std::make_unique<FixpointIterator>(
      cg,
      AnalyzerGenerator(immut_analyzer_state, api_level_analyzer_state,
                        string_analyzer_state, package_name_state, cp_state),
      cg_for_wps);
  // Run the bootstrap. All field value and method return values are
  // represented by Top.
  fp_iter->run(Domain{{CURRENT_PARTITION_LABEL, ArgumentDomain()}});
  auto non_true_virtuals =
      mog::get_non_true_virtuals(*method_override_graph, scope);
  UnorderedSet<const DexField*> definitely_assigned_ifields;
  if (m_config.compute_definitely_assigned_ifields) {
    definitely_assigned_ifields =
        definitely_assigned_ifields::get_definitely_assigned_ifields(scope);
  }
  m_stats.definitely_assigned_ifields = definitely_assigned_ifields.size();
  for (size_t i = 0; i < m_config.max_heap_analysis_iterations; ++i) {
    // Build an approximation of all the field values and method return values.
    auto wps = std::make_unique<WholeProgramState>(
        scope, *fp_iter, non_true_virtuals, m_config.field_blocklist,
        definitely_assigned_ifields, cg_for_wps);
    // If this approximation is not better than the previous one, we are done.
    if (fp_iter->get_whole_program_state().leq(*wps)) {
      break;
    }
    // Use the refined WholeProgramState to propagate more constants via
    // the stack and registers.
    fp_iter->set_whole_program_state(std::move(wps));
    fp_iter->run(Domain{{CURRENT_PARTITION_LABEL, ArgumentDomain()}});
  }
  compute_analysis_stats(fp_iter->get_whole_program_state(),
                         definitely_assigned_ifields);
  return fp_iter;
}

void PassImpl::compute_analysis_stats(
    const WholeProgramState& wps,
    const UnorderedSet<const DexField*>& definitely_assigned_ifields) {
  if (!wps.get_field_partition().is_top()) {
    for (auto& pair : wps.get_field_partition().bindings()) {
      auto* field = pair.first;
      auto& value = pair.second;
      if (value.is_top() || !is_useful(field->get_type(), value)) {
        continue;
      }
      if (definitely_assigned_ifields.count(field)) {
        ++m_stats.constant_definitely_assigned_ifields;
        TRACE(ICONSTP, 4, "definitely assigned field partition for %s: %s",
              SHOW(field), SHOW(value));
      } else {
        TRACE(ICONSTP, 4, "field partition for %s: %s", SHOW(field),
              SHOW(value));
      }
      ++m_stats.constant_fields;
    }
  }
  if (!wps.get_method_partition().is_top()) {
    for (auto& pair : wps.get_method_partition().bindings()) {
      auto* method = pair.first;
      auto& value = pair.second;
      if (value.is_top() ||
          !is_useful(method->get_proto()->get_rtype(), value)) {
        continue;
      }
      TRACE(ICONSTP, 4, "method partition for %s: %s", SHOW(method),
            SHOW(value));
      ++m_stats.constant_methods;
    }
  }
}

/*
 * Transform all methods using the information about constant method arguments
 * that analyze() obtained.
 */
void PassImpl::optimize(
    const Scope& scope,
    const TypeSystem& type_system,
    const XStoreRefs& xstores,
    const FixpointIterator& fp_iter,
    const ImmutableAttributeAnalyzerState* immut_analyzer_state,
    const State& cp_state) {
  const auto& pure_methods = ::get_pure_methods();
  m_transform_stats =
      walk::parallel::methods<Transform::Stats>(scope, [&](DexMethod* method) {
        if (method->get_code() == nullptr ||
            method->rstate.no_optimizations()) {
          return Transform::Stats();
        }
        auto& code = *method->get_code();
        auto ipa = fp_iter.get_intraprocedural_analysis(method);

        if (m_config.create_runtime_asserts) {
          RuntimeAssertTransform rat(m_config.runtime_assert);
          rat.apply(ipa->fp_iter, fp_iter.get_whole_program_state(), method);
          return Transform::Stats();
        } else {
          Transform::Config config(m_config.transform);
          config.class_under_init =
              method::is_clinit(method) ? method->get_class() : nullptr;
          config.getter_methods_for_immutable_fields =
              &immut_analyzer_state->attribute_methods;
          config.pure_methods = &pure_methods;
          Transform tf(config, cp_state);
          tf.legacy_apply_constants_and_prune_unreachable(
              ipa->fp_iter,
              fp_iter.get_whole_program_state(),
              code.cfg(),
              &xstores,
              method->get_class());
          // If configured, plug in IPCP state to do additional transforms
          // (API unwrapping to primitives for known ObjectWithImmutAttr
          // instances).
          wrapped_primitives::optimize_method(type_system, ipa->fp_iter,
                                              fp_iter.get_whole_program_state(),
                                              method, code.cfg());
          return tf.get_stats();
        }
      });
}

void PassImpl::run(const DexStoresVector& stores,
                   int min_sdk,
                   const boost::optional<std::string>& package_name) {
  // reset statistics, to be meaningful when pass runs multiple times
  m_stats = Stats();
  m_transform_stats = Transform::Stats();

  auto scope = build_class_scope(stores);
  XStoreRefs xstores(stores);

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    always_assert(code.editable_cfg_built());
    code.cfg().calculate_exit_block();
  });

  // Hold the analyzer state of ImmutableAttributeAnalyzer.
  ImmutableAttributeAnalyzerState immut_analyzer_state;
  immutable_state::analyze_constructors(scope, &immut_analyzer_state);
  ApiLevelAnalyzerState api_level_analyzer_state =
      ApiLevelAnalyzerState::get(min_sdk);
  auto string_analyzer_state = StringAnalyzerState::get();
  auto package_name_state = PackageNameState::get(package_name);
  State cp_state;
  auto fp_iter =
      analyze(scope, &immut_analyzer_state, &api_level_analyzer_state,
              &string_analyzer_state, &package_name_state, cp_state);
  m_stats.fp_iter = fp_iter->get_stats();
  TypeSystem type_system(scope);
  optimize(scope, type_system, xstores, *fp_iter, &immut_analyzer_state,
           cp_state);
}

void PassImpl::eval_pass(DexStoresVector& stores,
                         ConfigFiles& conf,
                         PassManager&) {
  auto string_analyzer_state = StringAnalyzerState::get();
  string_analyzer_state.set_methods_as_root();
}

void PassImpl::run_pass(DexStoresVector& stores,
                        ConfigFiles& config,
                        PassManager& mgr) {
  if (m_config.create_runtime_asserts) {
    m_config.runtime_assert =
        RuntimeAssertTransform::Config(config.get_proguard_map());
  }

  auto& options = mgr.get_redex_options();
  run(stores, options.min_sdk, options.package_name);

  ScopedMetrics sm(mgr);
  m_transform_stats.log_metrics(sm, /* with_scope= */ false);

  mgr.incr_metric("definitely_assigned_ifields",
                  m_stats.definitely_assigned_ifields);
  mgr.incr_metric("constant_definitely_assigned_ifields",
                  m_stats.constant_definitely_assigned_ifields);
  mgr.incr_metric("constant_fields", m_stats.constant_fields);
  mgr.incr_metric("constant_methods", m_stats.constant_methods);
  mgr.incr_metric("callgraph_edges", m_stats.callgraph_edges);
  mgr.incr_metric("callgraph_nodes", m_stats.callgraph_nodes);
  mgr.incr_metric("callgraph_callsites", m_stats.callgraph_callsites);
  mgr.incr_metric("fp_iter.method_cache_hits",
                  m_stats.fp_iter.method_cache_hits);
  mgr.incr_metric("fp_iter.method_cache_misses",
                  m_stats.fp_iter.method_cache_misses);
}

static PassImpl s_pass;

} // namespace interprocedural

} // namespace constant_propagation
