/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OpcodeList.h"

std::vector<DexOpcode> all_dex_opcodes{
#define OP(op, ...) DOPCODE_##op,
    DOPS
#undef OP
};

std::vector<IROpcode> all_opcodes{
#define OP(op, ...) OPCODE_##op,
#define IOP(...)
#define OPRANGE(...)
#include "IROpcodes.def"
};
