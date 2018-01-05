/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/algorithm/string/predicate.hpp>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "ImmutableSubcomponentAnalyzer.h"
#include "JarLoader.h"
#include "RedexContext.h"

std::vector<std::vector<std::string>> expected_paths = {
    // First call to `check`
    {"p0.getA()",
     "p0.getA().getB()",
     "p0.getA().getC()",
     "p0.getB().getD()",
     "p0.getA().getB().getD().getE()"},
    // Second call to `check`
    {"p1.getA()",
     "p1.getB()",
     "p1.getA().getC()",
     "p1.getB().getD()",
     "p1.getB().getD().getE()"},
    // Third call to `check`
    {"?", "?", "?", "?", "?"}};

void validate_arguments(size_t occurrence,
                        IRInstruction* insn,
                        const ImmutableSubcomponentAnalyzer& analyzer) {
  std::vector<std::string> args;
  for (size_t i = 0; i < 5; ++i) {
    auto path_opt = analyzer.get_access_path(insn->src(i), insn);
    args.push_back(path_opt ? path_opt->to_string() : "?");
  }
  EXPECT_THAT(args, ::testing::ElementsAreArray(expected_paths[occurrence]));
}

bool is_immutable_getter(DexMethodRef* method) {
  return boost::algorithm::starts_with(method->get_name()->str(), "get");
}

TEST(ImmutableSubcomponentAnalyzerTest, accessPaths) {
  g_redex = new RedexContext();

  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);

  const char* dexfile = std::getenv("dexfile");
  ASSERT_NE(nullptr, dexfile);
  root_store.add_classes(load_classes_from_dex(dexfile));
  stores.emplace_back(std::move(root_store));

  const char* android_sdk = std::getenv("ANDROID_SDK");
  ASSERT_NE(nullptr, android_sdk);
  const char* android_target = std::getenv("android_target");
  ASSERT_NE(nullptr, android_target);
  std::string android_version(android_target);
  ASSERT_NE("NotFound", android_version);
  std::string sdk_jar = std::string(android_sdk) + "/platforms/" +
                        android_version + "/android.jar";
  ASSERT_TRUE(load_jar_file(sdk_jar.c_str()));

  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  for (const auto& cls : scope) {
    if (cls->get_name()->str() ==
        "Lcom/facebook/redextest/ImmutableSubcomponentAnalyzer;") {
      for (const auto& method : cls->get_dmethods()) {
        if (method->get_name()->str() == "test") {
          ImmutableSubcomponentAnalyzer analyzer(method, is_immutable_getter);
          size_t check_occurrence = 0;
          for (auto& mie : InstructionIterable(method->get_code())) {
            IRInstruction* insn = mie.insn;
            if (insn->opcode() == OPCODE_INVOKE_STATIC &&
                insn->get_method()->get_name()->str() == "check") {
              validate_arguments(check_occurrence++, insn, analyzer);
            }
          }
        }
      }
    }
  }

  delete g_redex;
}
