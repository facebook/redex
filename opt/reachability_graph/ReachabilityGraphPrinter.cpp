/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReachabilityGraphPrinter.h"

#include <fstream>

#include "PassManager.h"

void ReachabilityGraphPrinterPass::run_pass(DexStoresVector& stores,
                                            ConfigFiles& /*cfg*/,
                                            PassManager& pm) {
  if (pm.no_proguard_rules()) {
    TRACE(RMU,
          1,
          "RemoveUnreachablePass not run because no "
          "ProGuard configuration was provided.");
    return;
  }

  auto reachables = compute_reachable_objects(
      stores, m_ignore_sets, nullptr, true /*generate graph*/);

  std::string tag = std::to_string(pm.get_current_pass_info()->repeat + 1);

  if (!m_output_file_name.empty()) {
    std::string file_name;
    if (pm.get_current_pass_info()->total_repeat == 1) {
      file_name = m_output_file_name;
    } else {
      file_name = m_output_file_name + "." + tag;
    }
    std::ofstream file;
    file.open(file_name);
    if (!file.is_open()) {
      std::cerr << "Unable to open: " << file_name << std::endl;
      exit(EXIT_FAILURE);
    }
    reachability::dump_graph(stores, reachables->retainers_of(), tag, file);
  }

  if (m_dump_detailed_info) {
    reachability::dump_info(
        stores, reachables->retainers_of(), "[" + tag + "]");
  }
}

static ReachabilityGraphPrinterPass s_pass;
