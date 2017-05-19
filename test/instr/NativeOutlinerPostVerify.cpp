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
#include "PassManager.h"
#include "RedexContext.h"
#include "Transform.h"

#include "NativeOutliner.h"
#include "NativeOutliner_generated.h"

using namespace facebook::redex::outliner;
namespace fs = boost::filesystem;

constexpr static const char ARTIFACTS_FILENAME[] = "redex-outliner-artifacts.bin";

TEST(PostVerify, native_outliner) {
  const char* apk = std::getenv("apk");
  fs::path artifacts_path(apk);
  artifacts_path.remove_filename();
  artifacts_path += fs::path::preferred_separator;
  // N.B. **very hardcoded** and must stay in sync w/ instr test buck
  artifacts_path += "native_outliner_redex__redex";
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
  ASSERT_GE(outlined_throws->outlined_throws()->size(), 2);

  bool foundOutlinedRuntimeException = false;
  bool foundOutlinedError = false;
  for (auto outlined_throw : *(outlined_throws->outlined_throws())) {
    if ("java.lang.RuntimeException" == outlined_throw->type()->str()) {
      foundOutlinedRuntimeException |= outlined_throw->msg()->str() == "Outlined RuntimeException __TEST__";
    } else if ("java.lang.Error" == outlined_throw->type()->str()) {
      foundOutlinedError |= outlined_throw->msg()->str() == "Outlined Error __TEST__";
    }
  }
  ASSERT_TRUE(foundOutlinedRuntimeException);
  ASSERT_TRUE(foundOutlinedError);

  delete [] data;
}
