/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <inttypes.h>

#pragma once

struct dex_stats_t {
  uint8_t signature[20];

  int64_t num_types = 0;
  int64_t num_classes = 0;
  int64_t num_methods = 0;
  int64_t num_method_refs = 0;
  int64_t num_fields = 0;
  int64_t num_field_refs = 0;
  int64_t num_strings = 0;
  int64_t num_protos = 0;
  int64_t num_static_values = 0;
  int64_t num_annotations = 0;
  int64_t num_type_lists = 0;
  int64_t num_bytes = 0;
  int64_t num_instructions = 0;
  int64_t num_callsites = 0;
  int64_t num_methodhandles = 0;
  int64_t num_tries = 0;

  int64_t num_unique_strings = 0;
  int64_t num_unique_types = 0;
  int64_t num_unique_protos = 0;
  int64_t num_unique_method_refs = 0;
  int64_t num_unique_field_refs = 0;

  int64_t strings_total_size = 0;
  int64_t types_total_size = 0;
  int64_t protos_total_size = 0;
  int64_t method_refs_total_size = 0;
  int64_t field_refs_total_size = 0;

  int64_t num_dbg_items = 0;
  int64_t dbg_total_size = 0;

  int64_t instruction_bytes = 0;

  /* Stats collected from the Map List section of a Dex. */
  int64_t header_item_count = 0;
  int64_t header_item_bytes = 0;

  int64_t string_id_count = 0;
  int64_t string_id_bytes = 0;

  int64_t type_id_count = 0;
  int64_t type_id_bytes = 0;

  int64_t proto_id_count = 0;
  int64_t proto_id_bytes = 0;

  int64_t field_id_count = 0;
  int64_t field_id_bytes = 0;

  int64_t method_id_count = 0;
  int64_t method_id_bytes = 0;

  int64_t class_def_count = 0;
  int64_t class_def_bytes = 0;

  int64_t call_site_id_count = 0;
  int64_t call_site_id_bytes = 0;

  int64_t method_handle_count = 0;
  int64_t method_handle_bytes = 0;

  int64_t map_list_count = 0;
  int64_t map_list_bytes = 0;

  int64_t type_list_count = 0;
  int64_t type_list_bytes = 0;

  int64_t annotation_set_ref_list_count = 0;
  int64_t annotation_set_ref_list_bytes = 0;

  int64_t annotation_set_count = 0;
  int64_t annotation_set_bytes = 0;

  int64_t class_data_count = 0;
  int64_t class_data_bytes = 0;

  int64_t code_count = 0;
  int64_t code_bytes = 0;

  int64_t string_data_count = 0;
  int64_t string_data_bytes = 0;

  int64_t debug_info_count = 0;
  int64_t debug_info_bytes = 0;

  int64_t annotation_count = 0;
  int64_t annotation_bytes = 0;

  int64_t encoded_array_count = 0;
  int64_t encoded_array_bytes = 0;

  int64_t annotations_directory_count = 0;
  int64_t annotations_directory_bytes = 0;

  int64_t class_order_violations = 0;

  dex_stats_t& operator+=(const dex_stats_t& rhs);
};
