/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Workflow:
//
// $ redex-tool dump-s-exprs \
//      --apkdir <APKDIR> --dexendir <DEXEN_DIR> \
//      --jars <ANDROID_JAR>
// (apkdir and jars may be empty)

#include <iostream>

#include "DexClass.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Show.h"
#include "Tool.h"

namespace {

void dump_method(const DexMethod* m) {
  auto code = m->get_code();
  std::cout << "(method (" << vshow((uint32_t)m->get_access(), true) << " \""
            << show(m) << "\"";
  if (code == nullptr) {
    std::cout << " NO CODE" << std::endl;
    return;
  }

  std::cout << std::endl
            << assembler::to_string(code) << std::endl
            << ")" << std::endl;
}

void dump_s_exprs(DexStoresVector& stores) {
  auto scope = build_class_scope(stores);
  for (auto* cls : scope) {
    std::cout << std::endl << "=== " << show(cls) << " ===" << std::endl;
    auto dump_methods = [](const auto& c) {
      for (auto* m : c) {
        std::cout << std::endl;
        dump_method(m);
      }
    };
    dump_methods(cls->get_dmethods());
    dump_methods(cls->get_vmethods());
  }
}

class DumpSExprs : public Tool {
 public:
  DumpSExprs()
      : Tool("dump-s-exprs", "dump dex bytecode to a list of s-exprs") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options); // For simplicity.
  }

  void run(const po::variables_map& options) override {
    auto stores = init(options["jars"].as<std::string>(),
                       options["apkdir"].as<std::string>(),
                       options["dexendir"].as<std::string>());
    dump_s_exprs(stores);
  }
};

static DumpSExprs s_tool;

} // namespace
