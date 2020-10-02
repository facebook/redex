/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Instrument.h"
#include "PassManager.h"

class BlockInstrumentHelper {
 public:
  static void do_basic_block_tracing(DexClass* analysis_cls,
                                     DexStoresVector& stores,
                                     ConfigFiles& cfg,
                                     PassManager& pm,
                                     const InstrumentPass::Options& options);
};
