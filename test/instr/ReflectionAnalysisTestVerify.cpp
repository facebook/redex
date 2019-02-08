/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ReflectionAnalysis.h"
#include "Show.h"
#include "verify/VerifyUtil.h"

std::string to_string(const reflection::ReflectionSites& reflection_sites) {
  std::ostringstream out;
  for (const auto it : reflection_sites) {
    out << SHOW(it.first) << " {";
    for (auto iit = it.second.begin(); iit != it.second.end(); ++iit) {
      out << iit->first << ", " << iit->second;
      if (std::next(iit) != it.second.end()) {
        out << ";";
      }
    }
    out << "}" << std::endl;
  }

  return out.str();
}

TEST_F(PostVerify, JoinClasssObjectSource) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/ReflectionAnalysisTest$Reflector;");
  ASSERT_NE(cls, nullptr);

  const auto meth = find_vmethod_named(*cls, "getClass");
  meth->balloon();
  reflection::ReflectionAnalysis analysis(meth);
  EXPECT_TRUE(analysis.has_found_reflection());
  EXPECT_EQ(
      to_string(analysis.get_reflection_sites()),
      "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, Ljava/lang/Class;.getPackage:()Ljava/lang/Package; {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n");
}
