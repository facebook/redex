/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UnconditionallyThrowingClassesPass.h"

#include <fstream>
#include <mutex>

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "PassManager.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
const std::string UNCONDITIONALLY_THROWING_CLASSES_FILENAME =
    "redex-unconditionally-throwing-classes.txt";
} // namespace

void UnconditionallyThrowingClassesPass::run_pass(DexStoresVector& stores,
                                                  ConfigFiles& conf,
                                                  PassManager& mgr) {
  auto scope = build_class_scope(stores);
  std::vector<DexClass*> throwing_classes;
  std::mutex throwing_classes_mutex;

  walk::parallel::classes(scope, [&](DexClass* cls) {
    auto* clinit = cls->get_clinit();
    if (clinit == nullptr) {
      return;
    }

    auto* code = clinit->get_code();
    if (code == nullptr) {
      return;
    }

    cfg::ScopedCFG cfg(code);

    if (cfg::block_eventually_throws(cfg->entry_block())) {
      std::lock_guard<std::mutex> lock(throwing_classes_mutex);
      throwing_classes.push_back(cls);
    }
  });

  // Sort for deterministic output
  std::sort(throwing_classes.begin(), throwing_classes.end(),
            compare_dexclasses);

  // Write unconditionally throwing classes to meta file
  std::string filepath =
      conf.metafile(UNCONDITIONALLY_THROWING_CLASSES_FILENAME);
  std::ofstream ofs(filepath);
  if (ofs.is_open()) {
    for (const auto* cls : throwing_classes) {
      ofs << show_deobfuscated(cls) << '\n';
    }
    ofs.close();
    TRACE(UNCONDITIONALLY_THROWING,
          1,
          "Wrote %zu unconditionally throwing classes to %s",
          throwing_classes.size(),
          filepath.c_str());
  } else {
    fprintf(stderr,
            "Unable to write unconditionally throwing classes to file %s\n",
            filepath.c_str());
  }

  mgr.set_metric("num_unconditionally_throwing_classes",
                 throwing_classes.size());
  TRACE(UNCONDITIONALLY_THROWING,
        1,
        "Found %zu classes with unconditionally throwing <clinit>",
        throwing_classes.size());
}

static UnconditionallyThrowingClassesPass s_pass;
