/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

#include "DexClass.h"
#include "MethodOverrideGraph.h"
#include "Reachability.h"
#include "RedexTest.h"
#include "Show.h"
#include "TypeUtil.h"
#include "Walkers.h"

using namespace reachability;

namespace {

using Labelled = std::map<std::string, std::set<std::string>>;

constexpr const char* kKeepRule = R"(
  -keepclasseswithmembers public class RemovedReachabilityGraphTest {
    public void entry();
  }
)";

std::string label(const ReachableObject& obj) {
  if (obj.type == ReachableObjectType::CLASS) {
    return show(obj.cls);
  }
  if (obj.type == ReachableObjectType::FIELD) {
    return show(obj.field);
  }
  if (obj.type == ReachableObjectType::METHOD) {
    return show(obj.method);
  }
  return "<unexpected node kind>";
}

// node -> its predecessors, i.e. the removed objects that reference it.
Labelled as_labels(const ReachableObjectGraph& graph) {
  Labelled result;
  for (const auto& entry : UnorderedIterable(graph)) {
    auto& preds = result[label(entry.first)];
    for (const auto& pred : UnorderedIterable(entry.second)) {
      preds.insert(label(pred));
    }
  }
  return result;
}

} // namespace

class RemovedReachabilityGraphTest : public RedexIntegrationTest {
 protected:
  void SetUp() override {
    create_object_class();
    auto* cls = type_class(type::java_lang_Object());
    // To make the assertion in reachability analysis happy.
    cls->set_external();
  }

  // Marks from the keep rule above, then builds the removed graph at the point
  // the pass builds it: after marking, before anything mutates or sweeps.
  Labelled build_removed_graph() {
    auto pg_config =
        process_and_get_proguard_config(stores[0].get_dexen(), kKeepRule);
    EXPECT_TRUE(pg_config->ok);

    int num_ignore_check_strings = 0;
    IgnoreSets ig_sets;
    ReachableAspects reachable_aspects;
    auto scope = build_class_scope(stores);
    walk::parallel::code(scope, [&](auto*, auto& code) { code.build_cfg(); });
    auto mog = method_override_graph::build_graph(scope);
    auto reachables = compute_reachable_objects(
        scope, *mog, ig_sets, &num_ignore_check_strings, &reachable_aspects);

    auto graph = compute_removed_reachability_graph(scope, *reachables);
    walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });
    return as_labels(graph);
  }
};

TEST_F(RemovedReachabilityGraphTest, EndToEndRemovedGraphSemantics) {
  auto graph = build_removed_graph();

  // A removed class is a root: containment points away from it, and the trivial
  // member-to-own-class reference is suppressed.
  EXPECT_EQ(graph["LDeadOwner;"], std::set<std::string>{});
  EXPECT_EQ(graph["LDeadOwner;.run:()V"], std::set<std::string>{"LDeadOwner;"});

  // A direct reference between removed objects, alongside containment from the
  // target's own class.
  EXPECT_EQ(graph["LDeadTarget;.staticMethod:()V"],
            (std::set<std::string>{"LDeadOwner;.run:()V", "LDeadTarget;"}));

  // A removed member of a retained class: the owner is not a node, so nothing
  // roots the member and it is a root itself.
  EXPECT_EQ(graph.count("LKept;"), 0u);
  EXPECT_EQ(graph["LKept;.unusedMember:()V"], std::set<std::string>{});

  // Compiled annotation element values are the one semantic the unit fixture
  // cannot reproduce: the value reaches a removed class as a CLASS node, while
  // the retained annotation class itself stays out.
  EXPECT_EQ(graph["LDeadViaAnno;"], std::set<std::string>{"LDeadAnnotated;"});
  EXPECT_EQ(graph.count("LDeadAnno;"), 0u);
}
