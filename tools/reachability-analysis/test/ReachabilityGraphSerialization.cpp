/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Reachability.h"

#include <fstream>

#include "Creators.h"
#include "Debug.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "RedexContext.h"
#include "SanitizersConfig.h"

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

  graph->emplace(cls, ReachableObjectSet{seed});
  graph->emplace(anno, ReachableObjectSet{cls});
  graph->emplace(method, ReachableObjectSet{cls});
  graph->emplace(field, ReachableObjectSet{method});
  return graph;
}

int main(int argc, char** argv) {
  always_assert(argc == 2);
  const auto* outfile = argv[1];

  g_redex = new RedexContext();

  const auto& graph = generate_graph();
  std::ofstream os;
  os.open(outfile);
  dump_graph(os, *graph);

  delete g_redex;

  return 0;
}
