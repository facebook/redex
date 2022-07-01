/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexCommon.h"

void dump_strings(ddump_data* rd, bool print_headers);
void dump_stringdata(ddump_data* rd, bool print_headers);
void dump_types(ddump_data* rd);
void dump_protos(ddump_data* rd, bool print_headers);
void dump_fields(ddump_data* rd, bool print_headers);
void dump_methods(ddump_data* rd, bool print_headers);
void dump_methodhandles(ddump_data* rd, bool print_headers);
void dump_callsites(ddump_data* rd, bool print_headers);
void dump_clsdefs(ddump_data* rd, bool print_headers);
void dump_clsdata(ddump_data* rd, bool print_headers);
void dump_code(ddump_data* rd);
void dump_enarr(ddump_data* rd);
void dump_anno(ddump_data* rd);
void dump_debug(ddump_data* rd);
void disassemble_debug(ddump_data* rd, uint32_t offset);
