/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumConfig.h"

#include "LocalPointersAnalysis.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"
#include <utility>

#include "KotlinNullCheckMethods.h"

namespace ptrs = local_pointers;

namespace {
// The structure is used for hardcoding external method param summaries.
struct ExternalMethodData {
  std::string method_name;
  boost::optional<uint16_t> returned_param;
  UnorderedSet<uint16_t> safe_params;
  ExternalMethodData(std::string name,
                     boost::optional<uint16_t> returned,
                     std::initializer_list<uint16_t> params)
      : method_name(std::move(name)),
        returned_param(returned),
        safe_params(params) {}
};

void sanity_check_method_summary(const DexMethodRef* method,
                                 const optimize_enums::ParamSummary& summary,
                                 const DexType* object_type) {
  auto args = method->get_proto()->get_args();
  for (auto param : UnorderedIterable(summary.safe_params)) {
    always_assert_log(param < args->size() &&
                          *(args->begin() + param) == object_type,
                      "%u is not Object;\n", param);
  }
  if (summary.returned_param) {
    always_assert(summary.safe_params.count(*summary.returned_param) &&
                  method->get_proto()->get_rtype() == object_type);
  }
  summary.print(method);
  if (method->is_def() && !method->is_external()) {
    // To improve:
    // 1. `equals` method is final on Integer and Enum, we can specialize equals
    // call.
    // 2. Run the method summary analysis muliple times until no new discovery.
    TRACE(ENUM, 9, "%s is not external but its method summary is hardcoded",
          SHOW(method));
    auto code = method->as_def()->get_code();
    TRACE(ENUM, 9, "%s", (code ? SHOW(code) : "**no code**"));
  }
}

/**
 * Hardcode some empirical summaries for some external methods.
 */
void load_external_method_summaries(
    const DexType* object_type, optimize_enums::SummaryMap* param_summary_map) {
  std::vector<ExternalMethodData> methods(
      {ExternalMethodData("Lcom/google/common/base/Objects;.equal:(Ljava/lang/"
                          "Object;Ljava/lang/"
                          "Object;)Z",
                          boost::none, {0, 1})});
  for (auto& item : methods) {
    auto method = DexMethod::get_method(item.method_name);
    if (!method) {
      continue;
    }
    always_assert(!param_summary_map->count(method));
    optimize_enums::ParamSummary summary(std::move(item.safe_params),
                                         item.returned_param);
    sanity_check_method_summary(method, summary, object_type);
    param_summary_map->emplace(method, summary);
  }
  // Load summaries for kotlin null assertion methods.
  auto null_check_methods =
      kotlin_nullcheck_wrapper::get_kotlin_null_assertions();
  for (auto method : UnorderedIterable(null_check_methods)) {
    if (param_summary_map->count(method)) {
      // The method is defined in the apk and their summaries are analyzed from
      // the method code.
      continue;
    }
    // Assume that kotlin assertions only check nullity of the first argument
    // and return void.
    optimize_enums::ParamSummary summary({0}, boost::none);
    always_assert(method->get_proto()->get_rtype() == type::_void());
    sanity_check_method_summary(method, summary, object_type);
    param_summary_map->emplace(method, summary);
  }
}
} // namespace

namespace optimize_enums {
void ParamSummary::print(const DexMethodRef* method) const {
  if (!traceEnabled(ENUM, 9)) {
    return;
  }
  TRACE(ENUM, 9, "summary of %s", SHOW(method));
  TRACE_NO_LINE(ENUM, 9, "safe_params: ");
  for (auto param : UnorderedIterable(safe_params)) {
    TRACE_NO_LINE(ENUM, 9, "%d ", param);
  }
  if (returned_param) {
    TRACE(ENUM, 9, "returned: %d", returned_param.get());
  } else {
    TRACE(ENUM, 9, "returned: none");
  }
}

/**
 * Return true if a method signature contains java.lang.Object type.
 */
bool params_contain_object_type(const DexMethod* method,
                                const DexType* object_type) {
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
  always_assert(code.cfg().editable());
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
        DexType* cmp = is_static(method) ? *(args->begin() + returned)
                       : returned == 0 ? method->get_class() // Implicit `this`
                                       : *(args->begin() + returned - 1);
        if (method->get_proto()->get_rtype() == cmp) {
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
  uint32_t index = 0;
  if (!is_static(method)) {
    index = 1;
  }
  for (; arg_it != args->end(); ++arg_it, ++index) {
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
void calculate_param_summaries(
    const Scope& scope,
    const method_override_graph::Graph& override_graph,
    SummaryMap* param_summary_map) {
  auto object_type = type::java_lang_Object();
  walk::parallel::code(
      scope,
      [object_type, &override_graph](DexMethod* method) {
        return method->get_code() &&
               !method_override_graph::is_true_virtual(override_graph,
                                                       method) &&

               params_contain_object_type(method, object_type);
      },
      [object_type, param_summary_map](DexMethod* method, IRCode&) {
        auto summary = calculate_param_summary(method, object_type);
        if (!summary.returned_param && summary.safe_params.empty()) {
          return;
        }
        param_summary_map->emplace(method, summary);
        summary.print(method);
      });
  TRACE(ENUM, 9, "External method summaries");
  load_external_method_summaries(object_type, param_summary_map);
}
} // namespace optimize_enums
