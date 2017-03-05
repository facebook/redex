/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Show.h"
#include "Tool.h"
#include "Transform.h"
#include "Walkers.h"

#include <queue>
#include <vector>
#include <unordered_map>

namespace {

void dump_viz(
  const Scope& scope,
  const char* cls_filter,
  const char* meth_filter,
  bool end_block_before_throw) {
  walk_methods(scope,
      [&](DexMethod* meth) {
        if (meth->get_code() == nullptr) return;
        if (cls_filter && !strstr(meth->get_class()->c_str(), cls_filter)) return;
        if (meth_filter && !strstr(meth->c_str(), meth_filter)) return;
        auto* mt = MethodTransform::get_method_transform(
          meth,
          true,
          end_block_before_throw);
        const auto& blocks = mt->cfg();
        fprintf(stderr, "digraph \"%s\" {\n", SHOW(meth));
        for (const auto& block : blocks) {
          fprintf(stderr, " \"%p\" [label=\"", block);
          for (auto mie = block->begin(); mie != block->end(); ++mie) {
            fprintf(stderr, " %s \\n ", SHOW(*mie));
          }
          fprintf(stderr, "\"]\n");
          for (const auto& succ : block->succs()) {
            fprintf(stderr, " \"%p\" -> \"%p\"\n", block, succ);
          }
        }
        fprintf(stderr, "}\n\n");
      });
}

}

class VizMflow : public Tool {
 public:
  VizMflow() : Tool("viz-mflow", "visualize method transforms") {}

  virtual void add_options(po::options_description& options) const {
    add_standard_options(options);
    options.add_options()
      ("class-filter,c",
       po::value<std::string>()->value_name("Lmy/pkg/foo"),
       "substring of class name to match")
      ("method-filter,m",
       po::value<std::string>()->value_name("get"),
       "substring of method name to match")
      ("end-block-before-throw,e",
       po::value<bool>()->value_name("true")->default_value(true),
       "should get_method_transform place end blocks before throw")
    ;
  }

  virtual void run(const po::variables_map& options) {
    auto stores = init(
      options["jars"].as<std::string>(),
      options["apkdir"].as<std::string>(),
      options["dexendir"].as<std::string>());
    const auto& scope = build_class_scope(stores);
    const char* class_filter = options.count("class-filter") ?
      options["class-filter"].as<std::string>().c_str() : nullptr;
    const char* method_filter = options.count("method-filter") ?
      options["method-filter"].as<std::string>().c_str() : nullptr;
    bool end_block_before_throw = options["end-block-before-throw"].as<bool>();
    dump_viz(scope, class_filter, method_filter, end_block_before_throw);
  }

 private:
};

static VizMflow s_tool;
