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
#include "IRCode.h"

using ResourceFiles = std::unordered_map<std::string, std::string>;
ResourceFiles decode_resource_paths(const char* location, const char* suffix);

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
  ResourceFiles resources;
  PreVerify()
      : Verify(),
        classes(load_classes_from_dex(std::getenv("dex_pre"),
                                      /* balloon */ false)),
        resources(decode_resource_paths(std::getenv("extracted_resources"),
                                        "_pre")) {}
};

struct PostVerify : public Verify {
  DexClasses classes;
  ResourceFiles resources;
  PostVerify()
      : Verify(),
        classes(load_classes_from_dex(std::getenv("dex_post"),
                                      /* balloon */ false)),
        resources(decode_resource_paths(std::getenv("extracted_resources"),
                                        "_post")) {}
};

DexClass* find_class_named(const DexClasses& classes, const char* name);
DexMethod* find_vmethod_named(const DexClass& cls, const char* name);
DexMethod* find_dmethod_named(const DexClass& cls, const char* name);
/* Find the first invoke instruction that calls a particular method name */
DexOpcodeMethod* find_invoke(const DexMethod* m, uint32_t opcode,
    const char* mname);
DexOpcodeMethod* find_invoke(
    std::vector<DexInstruction*>::iterator begin,
    std::vector<DexInstruction*>::iterator end,
    uint32_t opcode, const char* target_mname);
IRInstruction* find_instruction(DexMethod* m, uint32_t opcode);
IRInstruction* find_instruction(
    InstructionIterator begin,
    InstructionIterator end,
    uint32_t opcode);
