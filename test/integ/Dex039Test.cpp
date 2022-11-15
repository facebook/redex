/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

#include "DexCallSite.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "RedexTestUtils.h"
#include "SanitizersConfig.h"
#include "Show.h"
#include "Walkers.h"

TEST(Dex039Test, ReadDex039) {
  // const-method-handle.dex is sourced from https://fburl.com/prikp912
  // ground truth dexdump is found at https://fburl.com/27ekisha

  const char* dexfile = std::getenv("dex");
  EXPECT_NE(nullptr, dexfile);

  g_redex = new RedexContext();
  DexLoader dl(DexLocation::make_location("", dexfile));
  dex_stats_t stats{{0}};

  // bare minium test to ensure the dex loads okay
  auto classes = dl.load_dex(dexfile, &stats, 39);
  auto idx = dl.get_idx();

  // ensure that instructions can be shown
  std::ostringstream o;
  for (const auto& dex_class : classes) {
    o << show(dex_class);
    for (const auto& dex_method : dex_class->get_dmethods()) {
      o << show(dex_method);
      DexCode* code = dex_method->get_dex_code();
      for (const auto& dex_ins : code->get_instructions()) {
        o << show(dex_ins);
      }
    }
    for (const auto& dex_method : dex_class->get_vmethods()) {
      o << show(dex_method);
      DexCode* code = dex_method->get_dex_code();
      for (const auto& dex_ins : code->get_instructions()) {
        o << show(dex_ins);
      }
    }
  }
  auto parsed_code = o.str();

  EXPECT_NE(
      parsed_code.find("invoke-polymorphic "
                       "Ljava/lang/invoke/MethodHandle;.invokeExact:([Ljava/"
                       "lang/Object;)Ljava/lang/Object; v0, v5"),
      std::string::npos);
  EXPECT_NE(parsed_code.find("invoke-polymorphic "
                             "Ljava/lang/invoke/MethodHandle;.invoke:([Ljava/"
                             "lang/Object;)Ljava/lang/Object; v3, v2"),
            std::string::npos);
  EXPECT_NE(
      parsed_code.find("const-method-handle "
                       "Ljava/lang/Object;.getClass:()Ljava/lang/Class; v0"),
      std::string::npos);
  EXPECT_NE(
      parsed_code.find("const-method-type (CSIJFDLjava/lang/Object;)Z v0"),
      std::string::npos);
  EXPECT_EQ(idx->get_callsite_ids_size(), 0);
  EXPECT_EQ(idx->get_methodhandle_ids_size(), 1);
  EXPECT_EQ(idx->get_method_ids_size(), 23);
  EXPECT_EQ(idx->get_proto_ids_size(), 18);
  EXPECT_EQ(classes.size(), 2);
}
