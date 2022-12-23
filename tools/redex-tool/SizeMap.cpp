/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <iostream>

#include "DexOutput.h"
#include "Show.h"
#include "Tool.h"
#include "Walkers.h"

/*
 * This tool dumps method size and property information.
 *
 * Lcom/foo/bar;.<clinit>:()V, 20, 0, 0, 1
 * Lcom/foo/bar;.<init>:()V, 38, 0, 0, 1
 * Lcom/foo/bar;.enableSomething:(Landroid/content/Context;)V, 67, 0, 0, 1
 * ...
 */
namespace {
void dump_sizes(std::ostream& ofs, DexStoresVector& stores) {
  auto print = [&](DexMethod* method) {
    ofs << method->get_fully_deobfuscated_name() << ", "
        << (method->get_dex_code() ? method->get_dex_code()->size() : -1)
        << ", " << method->is_virtual() << ", " << method->is_external() << ", "
        << method->is_concrete() << std::endl;
  };
  walk::classes(build_class_scope(stores), [&](DexClass* cls) {
    for (auto dmethod : cls->get_dmethods()) {
      print(dmethod);
    }
    for (auto vmethod : cls->get_vmethods()) {
      print(vmethod);
    }
  });
}

class SizeMap : public Tool {
 public:
  SizeMap() : Tool("size-map", "dump sizes of methods") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options);
    options.add_options()(
        "rename-map,r",
        po::value<std::string>()->value_name("redex-rename-map.txt"),
        "path to a rename map")(
        "output,o",
        po::value<std::string>()->value_name("dex.sql"),
        "path to output size map file (defaults to stdout)");
  }

  void run(const po::variables_map& options) override {
    // Don't balloon. Need to get the code size.
    auto stores = init(
        options["jars"].as<std::string>(), options["apkdir"].as<std::string>(),
        options["dexendir"].as<std::string>(), false /* balloon */);

    ProguardMap pgmap(options.count("rename-map")
                          ? options["rename-map"].as<std::string>()
                          : "/dev/null");

    for (auto& store : stores) {
      auto& dexen = store.get_dexen();
      apply_deobfuscated_names(dexen, pgmap);
    }

    std::ofstream ofs;
    if (options.count("output")) {
      ofs.open(options["output"].as<std::string>(),
               std::ofstream::out | std::ofstream::trunc);
      dump_sizes(ofs, stores);
    } else {
      dump_sizes(std::cout, stores);
    }
  }
};

static SizeMap s_tool;

} // namespace
