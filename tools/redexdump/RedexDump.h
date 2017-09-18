/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexCommon.h"

void dump_strings(ddump_data* rd, bool print_headers);
void dump_stringdata(ddump_data* rd, bool print_headers);
void dump_types(ddump_data* rd);
void dump_protos(ddump_data* rd, bool print_headers);
void dump_fields(ddump_data* rd, bool print_headers);
void dump_methods(ddump_data* rd, bool print_headers);
void dump_clsdefs(ddump_data* rd, bool print_headers);
void dump_clsdata(ddump_data* rd, bool print_headers);
void dump_code(ddump_data* rd);
void dump_enarr(ddump_data* rd);
void dump_anno(ddump_data* rd);
void dump_debug(ddump_data* rd);
void disassemble_debug(ddump_data* rd, uint32_t offset);
