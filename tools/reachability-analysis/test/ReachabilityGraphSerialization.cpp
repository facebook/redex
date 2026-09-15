/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Reachability.h"

#include <fstream>
#include <sstream>
#include <unistd.h>

#include "Creators.h"
#include "Debug.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "RedexContext.h"

using namespace reachability;

std::unique_ptr<ReachableObjectGraph> generate_graph() {
  auto graph = std::make_unique<ReachableObjectGraph>();
  auto seed = ReachableObject();

  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(type::java_lang_Object());
  auto cls = ReachableObject(cc.create());

  auto field = ReachableObject(DexField::make_field("LFoo;.field1:I"));
  auto method = ReachableObject(DexMethod::make_method("LFoo;.method1:()I"));
  auto anno = ReachableObject(
      new DexAnnotation(DexType::make_type("LAnno;"), DAV_RUNTIME));

  ClassCreator removed_root_cc(DexType::make_type("LRemovedRoot;"));
  removed_root_cc.set_super(type::java_lang_Object());
  auto removed_root = ReachableObject(removed_root_cc.create());

  ClassCreator removed_cls_cc(DexType::make_type("LRemovedChild;"));
  removed_cls_cc.set_super(type::java_lang_Object());
  auto removed_cls = ReachableObject(removed_cls_cc.create());

  graph->emplace(cls, ReachableObjectSet{seed});
  graph->emplace(anno, ReachableObjectSet{cls});
  graph->emplace(method, ReachableObjectSet{cls});
  graph->emplace(field, ReachableObjectSet{method});
  // A root with no predecessors, as the removed graph encodes them, plus one
  // child so the component is reachable from it.
  graph->emplace(removed_root, ReachableObjectSet{});
  graph->emplace(removed_cls, ReachableObjectSet{removed_root});
  return graph;
}

std::string serialize_graph(const ReachableObjectGraph& graph) {
  std::ostringstream os;
  dump_graph(os, graph);
  return os.str();
}

int main(int argc, char** argv) {
  always_assert(argc == 2);
  const auto* outfile = argv[1];

  g_redex = new RedexContext();

  const auto& graph = generate_graph();
  const auto serialized = serialize_graph(*graph);
  {
    std::ofstream os(outfile, std::ios::binary);
    os.write(serialized.data(),
             static_cast<std::streamsize>(serialized.size()));
    always_assert(os.good());
  }

  _exit(0); // Do not clean up.
}
