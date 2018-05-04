/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IPConstantPropagation.h"

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "IPConstantPropagationAnalysis.h"
#include "Timer.h"
#include "Walkers.h"

namespace constant_propagation {

namespace interprocedural {

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
std::unique_ptr<FixpointIterator> PassImpl::analyze(const Scope& scope) {
  call_graph::Graph cg = call_graph::single_callee_graph(scope);
  // Rebuild all CFGs here -- this should be more efficient than doing them
  // within FixpointIterator::analyze_node(), since that can get called
  // multiple times for a given method
  walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg();
    code.cfg().calculate_exit_block();
  });
  auto fp_iter =
      std::make_unique<FixpointIterator>(cg, m_config.intraprocedural_analysis);
  // Run the bootstrap. All field value and method return values are
  // represented by Top.
  fp_iter->run({{CURRENT_PARTITION_LABEL, ArgumentDomain()}});

  for (size_t i = 0; i < m_config.max_heap_analysis_iterations; ++i) {
    // Build an approximation of all the field values and method return values.
    auto wps = std::make_unique<WholeProgramState>(scope, *fp_iter);
    // If this approximation is not better than the previous one, we are done.
    if (fp_iter->get_whole_program_state().leq(*wps)) {
      break;
    }
    // Use the refined WholeProgramState to propagate more constants via
    // the stack and registers.
    fp_iter->set_whole_program_state(std::move(wps));
    fp_iter->run({{CURRENT_PARTITION_LABEL, ArgumentDomain()}});
  }
  compute_analysis_stats(fp_iter->get_whole_program_state());

  return fp_iter;
}

void PassImpl::compute_analysis_stats(const WholeProgramState& wps) {
  if (!wps.get_field_partition().is_top()) {
    for (auto& pair : wps.get_field_partition().bindings()) {
      auto* field = pair.first;
      auto& value = pair.second;
      if (value.is_top()) {
        continue;
      }
      // Since a boolean value can only have 1 and 0 as values, "GEZ" tells us
      // nothing useful about this field.
      if (is_boolean(field->get_type()) &&
          value.equals(SignedConstantDomain(sign_domain::Interval::GEZ))) {
        continue;
      }
      ++m_stats.constant_fields;
    }
  }
  if (!wps.get_method_partition().is_top()) {
    for (auto& pair : wps.get_method_partition().bindings()) {
      auto* method = pair.first;
      auto& value = pair.second;
      if (value.is_top()) {
        continue;
      }
      if (is_boolean(method->get_proto()->get_rtype()) &&
          value.equals(SignedConstantDomain(sign_domain::Interval::GEZ))) {
        continue;
      }
      ++m_stats.constant_methods;
    }
  }
}

/*
 * Transform all methods using the information about constant method arguments
 * that analyze() obtained.
 */
void PassImpl::optimize(const Scope& scope, const FixpointIterator& fp_iter) {
  using Data = std::nullptr_t;
  m_transform_stats = walk::parallel::reduce_methods<Data, Transform::Stats>(
      scope,
      [&](Data&, DexMethod* method) {
        if (method->get_code() == nullptr) {
          return Transform::Stats();
        }
        auto& code = *method->get_code();
        auto intra_cp = fp_iter.get_intraprocedural_analysis(method);

        if (m_config.create_runtime_asserts) {
          RuntimeAssertTransform rat(m_config.runtime_assert);
          rat.apply(*intra_cp, fp_iter.get_whole_program_state(), method);
          return Transform::Stats();
        } else {
          Transform tf(m_config.transform);
          return tf.apply(*intra_cp, fp_iter.get_whole_program_state(), &code);
        }
      },
      [](Transform::Stats a, Transform::Stats b) { // reducer
        return a + b;
      },
      [&](unsigned int) { // data initializer
        return nullptr;
      });
}

void PassImpl::run(Scope& scope) {
  auto fp_iter = analyze(scope);
  optimize(scope, *fp_iter);
}

void PassImpl::run_pass(DexStoresVector& stores,
                        ConfigFiles& config,
                        PassManager& mgr) {
  if (m_config.create_runtime_asserts) {
    m_config.runtime_assert =
        RuntimeAssertTransform::Config(config.get_proguard_map());
  }

  auto scope = build_class_scope(stores);
  run(scope);
  mgr.incr_metric("branches_removed", m_transform_stats.branches_removed);
  mgr.incr_metric("materialized_consts", m_transform_stats.materialized_consts);
  mgr.incr_metric("constant_fields", m_stats.constant_fields);
  mgr.incr_metric("constant_methods", m_stats.constant_methods);
}

static PassImpl s_pass;

} // namespace interprocedural

} // namespace constant_propagation
