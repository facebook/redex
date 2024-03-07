/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexStats.h"

dex_stats_t& dex_stats_t::operator+=(const dex_stats_t& rhs) {
  num_types += rhs.num_types;
  num_classes += rhs.num_classes;
  num_methods += rhs.num_methods;
  num_method_refs += rhs.num_method_refs;
  num_fields += rhs.num_fields;
  num_field_refs += rhs.num_field_refs;
  num_strings += rhs.num_strings;
  num_protos += rhs.num_protos;
  num_static_values += rhs.num_static_values;
  num_annotations += rhs.num_annotations;
  num_type_lists += rhs.num_type_lists;
  num_bytes += rhs.num_bytes;
  num_instructions += rhs.num_instructions;
  num_tries += rhs.num_tries;
  num_unique_types += rhs.num_unique_types;
  num_unique_protos += rhs.num_unique_protos;
  num_unique_strings += rhs.num_unique_strings;
  num_unique_method_refs += rhs.num_unique_method_refs;
  num_unique_field_refs += rhs.num_unique_field_refs;
  types_total_size += rhs.types_total_size;
  protos_total_size += rhs.protos_total_size;
  strings_total_size += rhs.strings_total_size;
  method_refs_total_size += rhs.method_refs_total_size;
  field_refs_total_size += rhs.field_refs_total_size;
  num_dbg_items += rhs.num_dbg_items;
  dbg_total_size += rhs.dbg_total_size;
  instruction_bytes += rhs.instruction_bytes;

  header_item_count += rhs.header_item_count;
  header_item_bytes += rhs.header_item_bytes;
  string_id_count += rhs.string_id_count;
  string_id_bytes += rhs.string_id_bytes;
  type_id_count += rhs.type_id_count;
  type_id_bytes += rhs.type_id_bytes;
  proto_id_count += rhs.proto_id_count;
  proto_id_bytes += rhs.proto_id_bytes;
  field_id_count += rhs.field_id_count;
  field_id_bytes += rhs.field_id_bytes;
  method_id_count += rhs.method_id_count;
  method_id_bytes += rhs.method_id_bytes;
  class_def_count += rhs.class_def_count;
  class_def_bytes += rhs.class_def_bytes;
  call_site_id_count += rhs.call_site_id_count;
  call_site_id_bytes += rhs.call_site_id_bytes;
  method_handle_count += rhs.method_handle_count;
  method_handle_bytes += rhs.method_handle_bytes;
  map_list_count += rhs.map_list_count;
  map_list_bytes += rhs.map_list_bytes;
  type_list_count += rhs.type_list_count;
  type_list_bytes += rhs.type_list_bytes;
  annotation_set_ref_list_count += rhs.annotation_set_ref_list_count;
  annotation_set_ref_list_bytes += rhs.annotation_set_ref_list_bytes;
  annotation_set_count += rhs.annotation_set_count;
  annotation_set_bytes += rhs.annotation_set_bytes;
  class_data_count += rhs.class_data_count;
  class_data_bytes += rhs.class_data_bytes;
  code_count += rhs.code_count;
  code_bytes += rhs.code_bytes;
  string_data_count += rhs.string_data_count;
  string_data_bytes += rhs.string_data_bytes;
  debug_info_count += rhs.debug_info_count;
  debug_info_bytes += rhs.debug_info_bytes;
  annotation_count += rhs.annotation_count;
  annotation_bytes += rhs.annotation_bytes;
  encoded_array_count += rhs.encoded_array_count;
  encoded_array_bytes += rhs.encoded_array_bytes;
  annotations_directory_count += rhs.annotations_directory_count;
  annotations_directory_bytes += rhs.annotations_directory_bytes;

  return *this;
}
