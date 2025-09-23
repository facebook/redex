/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <queue>
#include <unordered_map>
#include <vector>

#include "ControlFlow.h"
#include "IRCode.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Tool.h"
#include "Walkers.h"

namespace {

void dump_viz(const Scope& scope,
              const char* cls_filter,
              const char* meth_filter) {
  walk::code(scope, [&](DexMethod* meth, IRCode& code) {
    if ((cls_filter != nullptr) &&
        (strstr(meth->get_class()->c_str(), cls_filter) == nullptr)) {
      return;
    }
    if ((meth_filter != nullptr) &&
        (strstr(meth->c_str(), meth_filter) == nullptr)) {
      return;
    }
    cfg::ScopedCFG cfg(&code);
    const auto& blocks = cfg->blocks();
    fprintf(stderr, "digraph \"%s\" {\n", SHOW(meth));
    for (const auto& block : blocks) {
      fprintf(stderr, " \"%p\" [label=\"", block);
      for (auto mie = block->begin(); mie != block->end(); ++mie) {
        fprintf(stderr, " %s \\n ", SHOW(*mie));
      }
      fprintf(stderr, "\"]\n");
      for (const auto& succ : block->succs()) {
        fprintf(stderr, " \"%p\" -> \"%p\"\n", block, succ->target());
      }
    }
    fprintf(stderr, "}\n\n");
  });
}

} // namespace

class VizMflow : public Tool {
 public:
  VizMflow() : Tool("viz-mflow", "visualize method transforms") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options);
    /* clang-format off */
    options.add_options()
      ("class-filter,c",
       po::value<std::string>()->value_name("Lmy/pkg/foo"),
       "substring of class name to match")
      ("method-filter,m",
        po::value<std::string>()->value_name("get"),
        "substring of method name to match")
      ("end-block-before-throw,e",
        po::value<bool>()->value_name("true")->default_value(true),
        "should get_method_transform place end blocks before throw");
    /* clang-format on */
  }

  void run(const po::variables_map& options) override {
    auto stores = init(options["jars"].as<std::string>(),
                       options["apkdir"].as<std::string>(),
                       options["dexendir"].as<std::string>());
    const auto& scope = build_class_scope(stores);
    const char* class_filter =
        options.count("class-filter") != 0u
            ? options["class-filter"].as<std::string>().c_str()
            : nullptr;
    const char* method_filter =
        options.count("method-filter") != 0u
            ? options["method-filter"].as<std::string>().c_str()
            : nullptr;
    dump_viz(scope, class_filter, method_filter);
  }

 private:
};

static VizMflow s_tool;
