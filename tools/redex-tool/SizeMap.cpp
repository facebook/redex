/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ostream>

#include "DexOutput.h"
#include "Show.h"
#include "Tool.h"
#include "Walkers.h"

/*
 * This tool dumps method size and property information.
 *
 * C, N, Lcom/foo/bar;
 * M, N, <clinit>:()V, 20, 0, 0, 1
 * M, N, <init>:()V, 38, 0, 0, 1
 * M, N, enableSomething:(Landroid/content/Context;)V, 67, 0, 0, 1
 * ...
 */
namespace {
void dump_sizes(std::ostream& ofs, DexStoresVector& stores) {
  auto print = [&](DexMethod* method) {
    if (method->get_dex_code() == nullptr) {
      return;
    }
    ofs << "M, ";
    auto full_name = method->get_deobfuscated_name();
    // Print method name + proto, excluding class name.
    if (!full_name.empty()) {
      auto dot_pos = full_name.find(".");
      if (dot_pos != std::string::npos) {
        ofs << "N, " << full_name.substr(dot_pos + 1);
      } else {
        ofs << "?, " << full_name;
      }
    } else {
      ofs << "Y, " << method->c_str() << ":" << show(method->get_proto());
    }
    ofs << ", " << method->get_dex_code()->size() << ", "
        << method->is_virtual() << ", " << method->is_external() << ", "
        << method->is_concrete() << std::endl;
  };

  walk::classes(build_class_scope(stores), [&](DexClass* cls) {
    ofs << "C, "
        << (show_deobfuscated(cls).empty() ? ("Y, " + show(cls))
                                           : ("N, " + show_deobfuscated(cls)))
        << std::endl;
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
