/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexLoader.h"

struct Verify : testing::Test {
  Verify() {
    g_redex = new RedexContext();
  }
  ~Verify() {
    delete g_redex;
  }
};

struct PreVerify : public Verify {
  DexClasses classes;
  PreVerify():
    Verify(),
    classes(load_classes_from_dex(std::getenv("dex_pre"))) {}
};

struct PostVerify : public Verify {
  DexClasses classes;
  PostVerify():
    Verify(),
    classes(load_classes_from_dex(std::getenv("dex_post"))) {}
};

DexClass* find_class_named(const DexClasses& classes, const char* name);
DexMethod* find_vmethod_named(const DexClass& cls, const char* name);
DexInstruction* find_instruction(const DexMethod* m, uint32_t opcode);
