/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexContext.h"

struct RedexTest : public testing::Test {
  RedexTest() { g_redex = new RedexContext(); }

  ~RedexTest() { delete g_redex; }
};

/*
 * We compare IRCode objects by serializing them first. However, the serialized
 * forms lack newlines between instructions and so are rather difficult to read.
 * It's nice to print the original IRCode objects which have those newlines.
 *
 * This is a macro instead of a function so that the error messages will contain
 * the right line numbers.
 */
#define EXPECT_CODE_EQ(a, b)                                          \
  do {                                                                \
    IRCode* aCode = a;                                                \
    IRCode* bCode = b;                                                \
    std::string aStr = assembler::to_string(aCode);                   \
    std::string bStr = assembler::to_string(bCode);                   \
    if (aStr == bStr) {                                               \
      SUCCEED();                                                      \
    } else {                                                          \
      auto p = std::mismatch(aStr.begin(), aStr.end(), bStr.begin()); \
      FAIL() << '\n'                                                  \
             << "S-expressions failed to match: \n"                   \
             << aStr << '\n'                                          \
             << bStr << '\n'                                          \
             << std::string(p.first - aStr.begin(), '.') + "^\n"      \
             << "\nExpected:\n"                                       \
             << show(aCode) << "\nto be equal to:\n"                  \
             << show(bCode);                                          \
    }                                                                 \
  } while (0);
