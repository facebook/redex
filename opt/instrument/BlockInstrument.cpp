/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockInstrument.h"

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "MethodReference.h"
#include "Show.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::unordered_map<int, /*num of arguments*/ DexMethod*> build_onMethodExit_map(
    const DexClass& cls, const std::string& onMethodExit_name) {
  std::unordered_map<int, DexMethod*> onMethodExit_map;
  for (const auto& dm : cls.get_dmethods()) {
    if (onMethodExit_name == dm->get_name()->c_str()) {
      auto const v =
          std::next(dm->get_proto()->get_args()->get_type_list().begin(), 1);
      if (*v == DexType::make_type("[S")) {
        // General case: void onMethodExit(int offset, short[] bbVector)
        onMethodExit_map.emplace(1, dm);
      } else {
        // Specific cases: void onMethodExit(int offset, short vec1, ..., vecN)
        onMethodExit_map.emplace(dm->get_proto()->get_args()->size(), dm);
      }
    }
  }

  if (!onMethodExit_map.empty()) {
    return onMethodExit_map;
  }

  std::cerr << "[InstrumentPass] error: cannot find " << onMethodExit_name
            << " in " << show(cls) << std::endl;
  for (const auto& m : cls.get_dmethods()) {
    std::cerr << " " << show(m) << std::endl;
  }
  exit(1);
}

std::unordered_set<std::string> get_cold_start_classes(ConfigFiles& cfg) {
  auto interdex_list = cfg.get_coldstart_classes();
  std::unordered_set<std::string> cold_start_classes;
  std::string dex_end_marker0("LDexEndMarker0;");
  for (auto class_string : interdex_list) {
    if (class_string == dex_end_marker0) {
      break;
    }
    class_string.back() = '/';
    cold_start_classes.insert(class_string);
  }
  return cold_start_classes;
}
} // namespace

////////////////////////////////////////////////////////////////////////////////
// A simple basic block instrumentation algorithm using bit vectors:
//
// Original CFG:
//   +--------+       +--------+       +--------+
//   | block0 | ----> | block1 | ----> | block2 |
//   |        |       |        |       | Return |
//   +--------+       +--------+       +--------+
//
// This CFG is instrumented as following:
//  - Insert instructions to initialize bit vector(s) at the entry block.
//  - Set <bb_id>-th bit in the vector using or-lit/16. The bit vector is a
//    short type. There is no such or-lit/32 instruction.
//  - Before RETURN, insert INVOKE DynamicAnalysis.onMethodExit(method_id,
//    bit_vectors), where the recorded bit vectors are reported.
//
//   +------------------+     +------------------+     +-----------------------+
//   | * CONST v0, 0    | --> | * OR_LIT16 v0, 2 | --> | * OR_LIT16 v0, 4      |
//   | * OR_LIT16 v0, 1 |     |   block1         |     |   block2              |
//   |   block0         |     |                  |     | * CONST v2, method_id |
//   +------------------+     +------------------+     | * INVOKE v2,v0, ...   |
//                                                     |   Return              |
//                                                     +-----------------------+
//
////////////////////////////////////////////////////////////////////////////////
void BlockInstrumentHelper::do_basic_block_tracing(
    DexClass* analysis_cls,
    DexStoresVector& stores,
    ConfigFiles& cfg,
    PassManager& pm,
    const InstrumentPass::Options& options) {
  const auto& onMethodExit_map =
      build_onMethodExit_map(*analysis_cls, options.analysis_method_name);

  auto cold_start_classes = get_cold_start_classes(cfg);
  TRACE(INSTRUMENT, 7, "Cold start classes: %zu", cold_start_classes.size());

  size_t method_index = 1;
  int num_all_bbs = 0;
  int num_instrumented_bbs = 0;
  int num_instrumented_methods = 0;

  int eligibles = 0;
  int specials = 0;
  int picked_by_cs = 0;
  int picked_by_allowlist = 0;
  int blocklisted = 0;
  int rejected = 0;
  int candidates = 0;

  std::map<int /*id*/, std::pair<std::string /*name*/, int /*num_BBs*/>>
      method_id_name_map;
  std::map<size_t /*num_vectors*/, int /*count*/> bb_vector_stat;
  auto scope = build_class_scope(stores);
  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    if (method == analysis_cls->get_clinit()) {
      specials++;
      return;
    }
    if (std::any_of(onMethodExit_map.begin(),
                    onMethodExit_map.end(),
                    [&](const auto& e) { return e.second == method; })) {
      specials++;
      return;
    }

    eligibles++;
    if (!options.allowlist.empty() || options.only_cold_start_class) {
      if (InstrumentPass::is_included(method, options.allowlist)) {
        picked_by_allowlist++;
      } else if (InstrumentPass::is_included(method, cold_start_classes)) {
        picked_by_cs++;
      } else {
        rejected++;
        return;
      }
    }

    // Blocklist has priority over allowlist or cold start list.
    if (InstrumentPass::is_included(method, options.blocklist)) {
      blocklisted++;
      return;
    }

    candidates++;
    TRACE(INSTRUMENT, 9, "Candidate: %s", SHOW(method));

    // TODO: instrument_basic_blocks
  });

  TRACE(INSTRUMENT, 4, "Instrumentation candidates selection stats:");
  TRACE(INSTRUMENT, 4, "- All eligible methods: %d", eligibles);
  TRACE(INSTRUMENT, 4, "  (Special uninstrumentable methods: %d)", specials);
  TRACE(INSTRUMENT, 4, "- Not selected: %d", rejected);
  TRACE(INSTRUMENT, 4, "- Selected by allowlist: %d", picked_by_allowlist);
  TRACE(INSTRUMENT, 4, "- Selected by cold start set: %d", picked_by_cs);
  TRACE(INSTRUMENT, 4, "  (But rejected by blocklist: %d)", blocklisted);
  TRACE(INSTRUMENT, 4, "- Total candidates: %d", candidates);
}
