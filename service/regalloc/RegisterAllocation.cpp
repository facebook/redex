/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RegisterAllocation.h"

#include <iostream>

#include "DebugUtils.h"
#include "DexClass.h"
#include "GraphColoring.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "LiveRange.h"
#include "Show.h"
#include "Trace.h"

namespace regalloc {
namespace graph_coloring {

using Config = Allocator::Config;
using Stats = Allocator::Stats;

Stats allocate(const Config& allocator_config, DexMethod* method) {
  return allocate(allocator_config,
                  method->get_code(),
                  is_static(method),
                  [method]() { return show(method); });
}

Allocator::Stats allocate(
    const Config& allocator_config,
    IRCode* code,
    bool is_static,
    const std::function<std::string()>& method_describer) {
  if (code == nullptr) {
    return Stats();
  }

  TRACE(REG, 5, "regs:%u code:\n%s", code->get_registers_size(), SHOW(code));
  try {
    live_range::renumber_registers(code, /* width_aware */ true);
    // The transformations below all require a CFG.
    always_assert_log(code->cfg_built(), "Need cfg here\n");
    auto& cfg = code->cfg();
    Allocator allocator(allocator_config);
    allocator.allocate(cfg, is_static);
    cfg.recompute_registers_size();
    TRACE(REG, 5, "After alloc: regs:%u code:\n%s", cfg.get_registers_size(),
          ::SHOW(cfg));
    return allocator.get_stats();
  } catch (const std::exception& e) {
    std::cerr << "Failed to allocate " << method_describer() << ": " << e.what()
              << '\n';
    print_stack_trace(std::cerr, e);

    std::string cfg_tmp;
    if (code->cfg_built()) {
      cfg_tmp = SHOW(code->cfg());
      code->clear_cfg();
    }
    std::cerr << "As s-expr: " << '\n' << assembler::to_s_expr(code) << '\n';
    std::cerr << "As CFG: " << '\n' << cfg_tmp << '\n';

    throw;
  }
}

} // namespace graph_coloring
} // namespace regalloc
