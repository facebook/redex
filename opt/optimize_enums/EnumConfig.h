/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"

namespace optimize_enums {
/**
 * A summary of how the parameters are used in a method.
 */
struct ParamSummary {
  // Indices of parameters that do not escape through other ways except escaping
  // as return value.
  std::unordered_set<uint16_t> safe_params;
  // Index of parameter if it's exactly the return value.
  boost::optional<uint16_t> returned_param;

  ParamSummary() : returned_param(boost::none) {}

  ParamSummary(std::unordered_set<uint16_t>&& safe_params,
               boost::optional<uint16_t> returned_param)
      : safe_params(std::move(safe_params)), returned_param(returned_param) {}

  void print(const DexMethodRef* method) const;
};

using SummaryMap = ConcurrentMap<const DexMethodRef*, ParamSummary>;

struct Config {
  /**
   * We create a helper class `EnumUtils` in primary dex with all the boxed
   * integer fields for representing enum values. The maximum number of the
   * fields is equal to largest number of values of candidate enum classes. To
   * limit the size of the class, exclude the enum classes that contain more
   * than max_enum_size values before the transformation.
   */
  uint32_t max_enum_size;
  bool skip_sanity_check{false};
  /**
   * Will try to optimize the enums in the allowlist without considering
   * reference equality of the enum objects.
   */
  std::unordered_set<DexType*> breaking_reference_equality_allowlist;
  SummaryMap param_summary_map;
  ConcurrentSet<DexType*> candidate_enums;

  explicit Config(uint32_t max_size) : max_enum_size(max_size) {}

  explicit Config(uint32_t max_size,
                  bool skip_sanity_check,
                  const std::vector<DexType*>& allowlist)
      : max_enum_size(max_size),
        skip_sanity_check(skip_sanity_check),
        breaking_reference_equality_allowlist(allowlist.begin(),
                                              allowlist.end()) {}
};

bool params_contain_object_type(const DexMethod* method,
                                const DexType* object_type);

/**
 * Apply escape analysis on the method and transfrom the escape summary to param
 * summary.
 */
ParamSummary calculate_param_summary(DexMethod* method,
                                     const DexType* object_type);

void calculate_param_summaries(
    const Scope& scope,
    const method_override_graph::Graph& override_graph,
    SummaryMap* param_summary_map);
} // namespace optimize_enums
