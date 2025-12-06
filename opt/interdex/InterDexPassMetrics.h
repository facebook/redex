/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace interdex {

constexpr const char* METRIC_COLD_START_SET_DEX_COUNT =
    "cold_start_set_dex_count";
constexpr const char* METRIC_SCROLL_SET_DEX_COUNT = "scroll_set_dex_count";

constexpr const char* METRIC_REORDER_CLASSES = "num_reorder_classes";
constexpr const char* METRIC_REORDER_RESETS = "num_reorder_resets";
constexpr const char* METRIC_REORDER_REPRIORITIZATIONS =
    "num_reorder_reprioritization";
constexpr const char* METRIC_REORDER_CLASSES_SEEDS = "reorder_classes_seeds";

constexpr const char* METRIC_CLASSES_ADDED_FOR_RELOCATED_METHODS =
    "num_classes_added_for_relocated_methods";
constexpr const char* METRIC_RELOCATABLE_STATIC_METHODS =
    "num_relocatable_static_methods";
constexpr const char* METRIC_RELOCATABLE_NON_STATIC_DIRECT_METHODS =
    "num_relocatable_non_static_direct_methods";
constexpr const char* METRIC_RELOCATABLE_VIRTUAL_METHODS =
    "num_relocatable_virtual_methods";
constexpr const char* METRIC_RELOCATED_STATIC_METHODS =
    "num_relocated_static_methods";
constexpr const char* METRIC_RELOCATED_NON_STATIC_DIRECT_METHODS =
    "num_relocated_non_static_direct_methods";
constexpr const char* METRIC_RELOCATED_VIRTUAL_METHODS =
    "num_relocated_virtual_methods";
constexpr const char* METRIC_CURRENT_CLASSES_WHEN_EMITTING_REMAINING =
    "num_current_classes_when_emitting_remaining";

constexpr const char* METRIC_LINEAR_ALLOC_LIMIT = "linear_alloc_limit";
constexpr const char* METRIC_RESERVED_FREFS = "reserved_frefs";
constexpr const char* METRIC_RESERVED_TREFS = "reserved_trefs";
constexpr const char* METRIC_RESERVED_MREFS = "reserved_mrefs";
constexpr const char* METRIC_EMIT_CANARIES = "emit_canaries";
constexpr const char* METRIC_ORDER_INTERDEX = "order_interdex";

} // namespace interdex
