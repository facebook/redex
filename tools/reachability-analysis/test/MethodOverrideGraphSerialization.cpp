/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodOverrideGraph.h"

#include <fstream>

#include "Creators.h"
#include "Debug.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "RedexContext.h"
#include "SanitizersConfig.h"

namespace mog = method_override_graph;

std::unique_ptr<mog::Graph> generate_graph() {
  auto m1 = DexMethod::make_method("LFoo;.bar:()V")
                ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
  auto m2 = DexMethod::make_method("LBar;.bar:()V")
                ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
  auto m3 = DexMethod::make_method("LBaz;.bar:()V")
                ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
  auto m4 = DexMethod::make_method("LQux;.bar:()V")
                ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);

  auto graph = std::make_unique<mog::Graph>();
  graph->add_edge(m1, m2);
  graph->add_edge(m1, m3);
  graph->add_edge(m2, m4);
  graph->add_edge(m3, m4);

  return graph;
}

int main(int argc, char** argv) {
  always_assert(argc == 2);
  const auto* outfile = argv[1];

  g_redex = new RedexContext();

  const auto& graph = generate_graph();
  std::ofstream os;
  os.open(outfile);
  graph->dump(os);

  delete g_redex;

  return 0;
}
