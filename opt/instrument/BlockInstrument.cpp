/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BlockInstrument.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "MethodReference.h"
#include "Show.h"
#include "TypeSystem.h"
#include "Walkers.h"

#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using OnMethodExitMap =
    std::map<size_t, // arity of vector arguments (excluding `int offset`)
             std::pair<DexMethod*, // onMethodExit
                       DexMethod*>>; // onMethodExit_Epilogue

OnMethodExitMap build_onMethodExit_map(const DexClass& cls,
                                       const std::string& onMethodExit_name) {
  const std::string epilogue_name = onMethodExit_name + "_Epilogue";
  std::map<size_t, std::pair<DexMethod*, DexMethod*>> onMethodExit_map;
  for (const auto& m : cls.get_dmethods()) {
    const auto& name = m->get_name()->str();
    if (onMethodExit_name != name && epilogue_name != name) {
      continue;
    }

    // onMethodExit must be (int offset) or (int, short vec1, ..., short vecN);
    const auto* args = m->get_proto()->get_args();
    if (args->size() == 0 ||
        *args->get_type_list().begin() != DexType::make_type("I") ||
        std::any_of(
            std::next(args->get_type_list().begin(), 1),
            args->get_type_list().end(),
            [](const auto& type) { return type != DexType::make_type("S"); })) {
      std::cerr << "[InstrumentPass] error: Proto type of onMethodExit must be "
                   "(int) or (int, short, ..., short), but it was "
                << show(m->get_proto()) << std::endl;
      exit(1);
    }

    auto& pair = onMethodExit_map[args->size() - 1];
    if (epilogue_name == name) {
      pair.second = m;
    } else {
      pair.first = m;
    }
  }

  if (onMethodExit_map.empty()) {
    std::cerr << "[InstrumentPass] error: cannot find " << onMethodExit_name
              << " in " << show(cls) << std::endl;
    for (const auto& m : cls.get_dmethods()) {
      std::cerr << " " << show(m) << std::endl;
    }
    exit(1);
  }

  // For all non-zero arities, both onMethodExit/_Epilogue must exist.
  if (std::any_of(
          begin(onMethodExit_map), end(onMethodExit_map), [](const auto& kv) {
            return kv.first != 0 &&
                   (kv.second.first == nullptr || kv.second.second == nullptr);
          })) {
    std::cerr << "[InstrumentPass] error: there must be a pair of onMethodExit "
                 "and onMethodExit_Epilogue for each overloaded type, except "
                 "for zero arity"
              << std::endl;
    for (const auto& kv : onMethodExit_map) {
      std::cerr << " arity: " << kv.first
                << ", onMethodExit: " << (kv.second.first ? "T" : "F")
                << ", onMethodExit_Epilogue: " << (kv.second.second ? "T" : "F")
                << std::endl;
    }
    exit(1);
  }

  return onMethodExit_map;
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
  // I'm too lazy to support sharding in block instrumentation. Future work.
  if (options.num_shards != 1 || options.num_stats_per_method != 0) {
    std::cerr << "[InstrumentPass] error: basic block profiling must have "
                 "num_shard = 1 and num_stats_per_method = 0"
              << std::endl;
    exit(1);
  }

  const auto& onMethodExit_map =
      build_onMethodExit_map(*analysis_cls, options.analysis_method_name);
  const size_t max_vector_arity = onMethodExit_map.rbegin()->first - 1;

  auto cold_start_classes = get_cold_start_classes(cfg);
  TRACE(INSTRUMENT, 7, "Cold start classes: %zu", cold_start_classes.size());

  size_t method_offset = 1;
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
  int instrumented = 0;

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
                    [&](const auto& e) {
                      return e.second.first == method ||
                             e.second.second == method;
                    })) {
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
  TRACE(INSTRUMENT, 4, "- Total instrumented: %d", instrumented);
}
