/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStats.h"

dex_stats_t& operator+=(dex_stats_t& lhs, const dex_stats_t& rhs) {
  lhs.num_types += rhs.num_types;
  lhs.num_classes += rhs.num_classes;
  lhs.num_methods += rhs.num_methods;
  lhs.num_method_refs += rhs.num_method_refs;
  lhs.num_fields += rhs.num_fields;
  lhs.num_field_refs += rhs.num_field_refs;
  lhs.num_strings += rhs.num_strings;
  lhs.num_protos += rhs.num_protos;
  lhs.num_static_values += rhs.num_static_values;
  lhs.num_annotations += rhs.num_annotations;
  lhs.num_type_lists += rhs.num_type_lists;
  lhs.num_bytes += rhs.num_bytes;
  lhs.num_instructions += rhs.num_instructions;
  lhs.num_unique_types += rhs.num_unique_types;
  lhs.num_unique_protos += rhs.num_unique_protos;
  lhs.num_unique_strings += rhs.num_unique_strings;
  lhs.num_unique_method_refs += rhs.num_unique_method_refs;
  lhs.num_unique_field_refs += rhs.num_unique_field_refs;
  lhs.types_total_size += rhs.types_total_size;
  lhs.protos_total_size += rhs.protos_total_size;
  lhs.strings_total_size += rhs.strings_total_size;
  lhs.method_refs_total_size += rhs.method_refs_total_size;
  lhs.field_refs_total_size += rhs.field_refs_total_size;
  lhs.num_dbg_items += rhs.num_dbg_items;
  lhs.dbg_total_size += rhs.dbg_total_size;
  lhs.instruction_bytes += rhs.instruction_bytes;

  lhs.header_item_count += rhs.header_item_count;
  lhs.header_item_bytes += rhs.header_item_bytes;
  lhs.string_id_count += rhs.string_id_count;
  lhs.string_id_bytes += rhs.string_id_bytes;
  lhs.type_id_count += rhs.type_id_count;
  lhs.type_id_bytes += rhs.type_id_bytes;
  lhs.proto_id_count += rhs.proto_id_count;
  lhs.proto_id_bytes += rhs.proto_id_bytes;
  lhs.field_id_count += rhs.field_id_count;
  lhs.field_id_bytes += rhs.field_id_bytes;
  lhs.method_id_count += rhs.method_id_count;
  lhs.method_id_bytes += rhs.method_id_bytes;
  lhs.class_def_count += rhs.class_def_count;
  lhs.class_def_bytes += rhs.class_def_bytes;
  lhs.call_site_id_count += rhs.call_site_id_count;
  lhs.call_site_id_bytes += rhs.call_site_id_bytes;
  lhs.method_handle_count += rhs.method_handle_count;
  lhs.method_handle_bytes += rhs.method_handle_bytes;
  lhs.map_list_count += rhs.map_list_count;
  lhs.map_list_bytes += rhs.map_list_bytes;
  lhs.type_list_count += rhs.type_list_count;
  lhs.type_list_bytes += rhs.type_list_bytes;
  lhs.annotation_set_ref_list_count += rhs.annotation_set_ref_list_count;
  lhs.annotation_set_ref_list_bytes += rhs.annotation_set_ref_list_bytes;
  lhs.annotation_set_count += rhs.annotation_set_count;
  lhs.annotation_set_bytes += rhs.annotation_set_bytes;
  lhs.class_data_count += rhs.class_data_count;
  lhs.class_data_bytes += rhs.class_data_bytes;
  lhs.code_count += rhs.code_count;
  lhs.code_bytes += rhs.code_bytes;
  lhs.string_data_count += rhs.string_data_count;
  lhs.string_data_bytes += rhs.string_data_bytes;
  lhs.debug_info_count += rhs.debug_info_count;
  lhs.debug_info_bytes += rhs.debug_info_bytes;
  lhs.annotation_count += rhs.annotation_count;
  lhs.annotation_bytes += rhs.annotation_bytes;
  lhs.encoded_array_count += rhs.encoded_array_count;
  lhs.encoded_array_bytes += rhs.encoded_array_bytes;
  lhs.annotations_directory_count += rhs.annotations_directory_count;
  lhs.annotations_directory_bytes += rhs.annotations_directory_bytes;

  return lhs;
}
