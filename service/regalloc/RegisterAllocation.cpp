/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RegisterAllocation.h"

#include <iostream>

#include "Debug.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "LiveRange.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace regalloc {
namespace graph_coloring {

using Config = Allocator::Config;
using Stats = Allocator::Stats;

Stats allocate(const Config& allocator_config, DexMethod* m) {
  if (m->get_code() == nullptr) {
    return Stats();
  }
  auto& code = *m->get_code();
  TRACE(REG, 5, "regs:%d code:\n%s", code.get_registers_size(), SHOW(&code));
  try {
    live_range::renumber_registers(&code, /* width_aware */ true);
    // The transformations below all require a CFG. Build it once
    // here instead of requiring each transform to build it.
    code.build_cfg(/* editable */ false);
    Allocator allocator(allocator_config);
    allocator.allocate(m);
    TRACE(REG, 5, "After alloc: regs:%d code:\n%s", code.get_registers_size(),
          SHOW(&code));
    return allocator.get_stats();
  } catch (const std::exception& e) {
    std::cerr << "Failed to allocate " << SHOW(m) << ": " << e.what()
              << std::endl;
    print_stack_trace(std::cerr, e);

    std::string cfg_tmp;
    if (code.cfg_built()) {
      cfg_tmp = SHOW(code.cfg());
      code.clear_cfg();
    }
    std::cerr << "As s-expr: " << std::endl
              << assembler::to_s_expr(&code) << std::endl;
    std::cerr << "As CFG: " << std::endl << cfg_tmp << std::endl;

    throw;
  }
}

} // namespace graph_coloring
} // namespace regalloc
