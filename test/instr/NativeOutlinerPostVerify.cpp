/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "VerifyUtil.h"

#include "NativeOutliner.h"
#include "NativeOutliner_generated.h"

using namespace facebook::redex::outliner;
namespace fs = boost::filesystem;

constexpr static const char ARTIFACTS_FILENAME[] = "redex-outliner-artifacts.bin";
constexpr static const char MSG_1[] = "this is a test";
constexpr static const char MSG_2[] = "this is another test";

/*
 * Verify that the artifacts contain the expected contents.
 */
TEST_F(PostVerify, native_outliner_artifacts) {
  const char* apk = std::getenv("apk");
  fs::path artifacts_path(apk);
  artifacts_path.remove_filename();
  artifacts_path += fs::path::preferred_separator;
  // N.B. **very hardcoded** and must stay in sync w/ instr test buck
  artifacts_path += "native_outliner_redex_unsigned_pre_so__redex";
  artifacts_path += fs::path::preferred_separator;
  artifacts_path += ARTIFACTS_FILENAME;
  FILE* fdin = fopen(artifacts_path.string().c_str(), "rb");
  EXPECT_TRUE(fdin != nullptr);
  fseek(fdin, 0L, SEEK_END);
  int length = ftell(fdin);
  fseek(fdin, 0L, SEEK_SET);
  char *data = new char[length];
  fread(data, sizeof(char), length, fdin);
  fclose(fdin);

  auto outlined_throws = GetOutlinedThrows(data);
  EXPECT_GE(outlined_throws->outlined_throws()->size(), 2);

  bool foundOutlinedRuntimeException = false;
  bool foundOutlinedError = false;
  for (auto outlined_throw : *(outlined_throws->outlined_throws())) {
    if ("java/lang/RuntimeException" == outlined_throw->type()->str()) {
      foundOutlinedRuntimeException |= outlined_throw->msg()->str() == MSG_1;
    } else if ("java/lang/IllegalArgumentException" == outlined_throw->type()->str()) {
      foundOutlinedError |= outlined_throw->msg()->str() == MSG_2;
    }
  }
  EXPECT_TRUE(foundOutlinedRuntimeException);
  EXPECT_TRUE(foundOutlinedError);

  delete [] data;
}

/*
 * Verify that the pre-outlined dexes contain the expected strings.
 */
TEST_F(PreVerify, native_outliner_pre) {
  auto msg_1_str = g_redex->get_string(MSG_1, 0);
  EXPECT_NE(nullptr, msg_1_str);

  auto msg_2_str = g_redex->get_string(MSG_2, 0);
  EXPECT_NE(nullptr, msg_2_str);
}

/*
 * Verify that the post-outlined dexes do NOT contain the expected strings.
 */
TEST_F(PostVerify, native_outliner_post) {
  auto msg_1_str = g_redex->get_string(MSG_1, 0);
  EXPECT_EQ(nullptr, msg_1_str);

  auto msg_2_str = g_redex->get_string(MSG_2, 0);
  EXPECT_EQ(nullptr, msg_2_str);
}
