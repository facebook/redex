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

  int num_types = 0;
  int num_classes = 0;
  int num_methods = 0;
  int num_method_refs = 0;
  int num_fields = 0;
  int num_field_refs = 0;
  int num_strings = 0;
  int num_protos = 0;
  int num_static_values = 0;
  int num_annotations = 0;
  int num_type_lists = 0;
  int num_bytes = 0;
  int num_instructions = 0;
  int num_callsites = 0;
  int num_methodhandles = 0;

  int num_unique_strings = 0;
  int num_unique_types = 0;
  int num_unique_protos = 0;
  int num_unique_method_refs = 0;
  int num_unique_field_refs = 0;

  int strings_total_size = 0;
  int types_total_size = 0;
  int protos_total_size = 0;
  int method_refs_total_size = 0;
  int field_refs_total_size = 0;

  int num_dbg_items = 0;
  int dbg_total_size = 0;

  int instruction_bytes = 0;

  /* Stats collected from the Map List section of a Dex. */
  int header_item_count = 0;
  int header_item_bytes = 0;

  int string_id_count = 0;
  int string_id_bytes = 0;

  int type_id_count = 0;
  int type_id_bytes = 0;

  int proto_id_count = 0;
  int proto_id_bytes = 0;

  int field_id_count = 0;
  int field_id_bytes = 0;

  int method_id_count = 0;
  int method_id_bytes = 0;

  int class_def_count = 0;
  int class_def_bytes = 0;

  int call_site_id_count = 0;
  int call_site_id_bytes = 0;

  int method_handle_count = 0;
  int method_handle_bytes = 0;

  int map_list_count = 0;
  int map_list_bytes = 0;

  int type_list_count = 0;
  int type_list_bytes = 0;

  int annotation_set_ref_list_count = 0;
  int annotation_set_ref_list_bytes = 0;

  int annotation_set_count = 0;
  int annotation_set_bytes = 0;

  int class_data_count = 0;
  int class_data_bytes = 0;

  int code_count = 0;
  int code_bytes = 0;

  int string_data_count = 0;
  int string_data_bytes = 0;

  int debug_info_count = 0;
  int debug_info_bytes = 0;

  int annotation_count = 0;
  int annotation_bytes = 0;

  int encoded_array_count = 0;
  int encoded_array_bytes = 0;

  int annotations_directory_count = 0;
  int annotations_directory_bytes = 0;
};

dex_stats_t& operator+=(dex_stats_t& lhs, const dex_stats_t& rhs);
