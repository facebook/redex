/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumConfig.h"
#include "LocalPointersAnalysis.h"
#include "Walkers.h"

namespace ptrs = local_pointers;

namespace optimize_enums {
void ParamSummary::print(const DexMethodRef* method) const {
  if (!traceEnabled(ENUM, 9)) {
    return;
  }
  TRACE(ENUM, 9, "summary of %s\n", SHOW(method));
  TRACE(ENUM, 9, "safe_params: ");
  for (auto param : safe_params) {
    TRACE(ENUM, 9, "%d ", param);
  }
  if (returned_param) {
    TRACE(ENUM, 9, "returned: %d\n", returned_param.get());
  } else {
    TRACE(ENUM, 9, "returned: none\n");
  }
}

/**
 * Return true if a static method signature contains java.lang.Object type.
 */
bool params_contain_object_type(const DexMethod* method,
                                const DexType* object_type) {
  if (!is_static(method) || !method->get_code()) {
    return false;
  }
  auto args = method->get_proto()->get_args();
  for (auto arg : *args) {
    if (arg == object_type) {
      return true;
    }
  }
  return false;
}

ParamSummary calculate_param_summary(DexMethod* method,
                                     const DexType* object_type) {
  auto& code = *method->get_code();
  code.build_cfg(/* editable */ false);
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  ParamSummary summary;

  ptrs::FixpointIterator fp_iter(cfg, ptrs::InvokeToSummaryMap(),
                                 /*escape_check_cast*/ true);
  fp_iter.run(ptrs::Environment());
  auto escape_summary = ptrs::get_escape_summary(fp_iter, code);
  if (escape_summary.returned_parameters.kind() ==
      sparta::AbstractValueKind::Top) {
    return summary;
  }

  auto args = method->get_proto()->get_args();
  auto& escaping_params = escape_summary.escaping_parameters;
  if (escape_summary.returned_parameters.kind() ==
      sparta::AbstractValueKind::Value) {
    auto& returned_elements = escape_summary.returned_parameters.elements();
    if (returned_elements.size() == 1) {
      auto returned = *returned_elements.begin();
      if (returned != ptrs::FRESH_RETURN && !escaping_params.count(returned)) {
        if (method->get_proto()->get_rtype() == *(args->begin() + returned)) {
          // Set returned_param to the only one returned parameter index.
          summary.returned_param = returned;
        } else {
          escaping_params.insert(returned);
        }
      }
    } else {
      // Treat as escaping if there are multiple returns.
      for (auto param : returned_elements) {
        escaping_params.insert(param);
      }
    }
  }
  // Non-escaping java.lang.Object params are stored in safe_params.
  auto arg_it = args->begin();
  for (uint32_t index = 0; arg_it != args->end(); ++arg_it, ++index) {
    if (!escaping_params.count(index) && (*arg_it == object_type)) {
      summary.safe_params.insert(index);
    }
  }
  return summary;
}

/**
 * Calculate escape summaries for static methods whose arguments contain
 * java.lang.Object type. Then convert the escape summaries to param summaries.
 */
void calculate_param_summaries(Scope& scope, SummaryMap* param_summary_map) {
  auto object_type = get_object_type();
  walk::parallel::code(
      scope,
      [object_type](DexMethod* method) {
        return params_contain_object_type(method, object_type);
      },
      [object_type, param_summary_map](DexMethod* method, IRCode&) {
        auto summary = calculate_param_summary(method, object_type);
        if (!summary.returned_param && summary.safe_params.empty()) {
          return;
        }
        param_summary_map->emplace(method, summary);
        summary.print(method);
      });
}
} // namespace optimize_enums
