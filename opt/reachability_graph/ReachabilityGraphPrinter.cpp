/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReachabilityGraphPrinter.h"

#include "PassManager.h"
#include "ReachableObjects.h"

#include <fstream>

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

  // A bit ugly copy from RMU pass, but...
  auto load_annos = [](const std::vector<std::string>& list) {
    std::unordered_set<const DexType*> set;
    for (const auto& name : list) {
      const auto type = DexType::get_type(name.c_str());
      if (type != nullptr) {
        set.insert(type);
      }
    }
    return set;
  };

  auto reachables =
      compute_reachable_objects(stores,
                                load_annos(m_ignore_string_literals),
                                load_annos(m_ignore_string_literal_annos),
                                load_annos(m_ignore_system_annos),
                                nullptr,
                                true /*generate graph*/);

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
    dump_reachability_graph(stores, reachables.retainers_of, tag, file);
  }

  if (m_dump_detailed_info) {
    dump_reachability(stores, reachables.retainers_of, "[" + tag + "]");
  }
}

static ReachabilityGraphPrinterPass s_pass;
