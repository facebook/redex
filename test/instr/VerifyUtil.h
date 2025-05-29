/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "IRCode.h"
#define IS_REDEX_TEST_LIBRARY
#include "RedexTest.h"
#undef IS_REDEX_TEST_LIBRARY

using ResourceFiles = std::unordered_map<std::string, std::string>;
ResourceFiles decode_resource_paths(const char* location);

struct PreVerify : public RedexTest {
  DexClasses classes;
  ResourceFiles resources;
  PreVerify()
      : classes(load_classes_from_dex(
            DexLocation::make_location("", std::getenv("dex_pre")),
            /*stats=*/nullptr,
            /* balloon */ false)),
        resources(
            decode_resource_paths(std::getenv("extracted_resources_pre"))) {}
};

struct PostVerify : public RedexTest {
  DexClasses classes;
  ResourceFiles resources;
  PostVerify()
      : classes(load_classes_from_dex(
            DexLocation::make_location("", std::getenv("dex_post")),
            /*stats=*/nullptr,
            /* balloon */ false)),
        resources(
            decode_resource_paths(std::getenv("extracted_resources_post"))) {}
};

// \returns -1 if no class with \name is found. Otherwise, \returns the the
// class's idx in \classes list.
int find_class_idx(const DexClasses& classes, const char* name);

DexClass* find_class_named(const DexClasses& classes, const char* name);
DexClass* find_class_named(const DexClasses& classes,
                           const std::function<bool(const char*)>& matcher);
DexField* find_ifield_named(const DexClass& cls, const char* name);
DexField* find_sfield_named(const DexClass& cls, const char* name);
DexField* find_field_named(const DexClass& cls, const char* name);
DexMethod* find_vmethod_named(const DexClass& cls, const char* name);
DexMethod* find_vmethod(const DexClass& cls,
                        const char* name,
                        const DexProto* proto);
DexMethod* find_dmethod_named(const DexClass& cls, const char* name);
DexMethod* find_method_named(const DexClass& cls, const char* name);
/* Find the first invoke instruction that calls a particular method name */
DexOpcodeMethod* find_invoke(const DexMethod* m,
                             DexOpcode opcode,
                             const char* mname,
                             DexType* receiver = nullptr);
DexOpcodeMethod* find_invoke(std::vector<DexInstruction*>::iterator begin,
                             std::vector<DexInstruction*>::iterator end,
                             DexOpcode opcode,
                             const char* target_mname,
                             DexType* receiver = nullptr);
DexInstruction* find_instruction(DexMethod* m, DexOpcode opcode);

/* Find the number of invoke instructions that calls a particular method name */
size_t find_num_invoke(const DexMethod* m,
                       DexOpcode opcode,
                       const char* target_mname,
                       DexType* receiver = nullptr);

void verify_class_merged(const DexClass* cls, size_t num_dmethods = 0);

// A quick helper to dump CFGs before/after verify
//
// How to use:
//  REDEX_INSTRUMENT_TEST_BASE_FILENAME="test.txt"
//  buck test //foo/test/instr:basic_block_tracing_verify
//
// You will see "before_test.txt" and "after_test.txt".
void dump_cfgs(bool is_prev_verify,
               const DexClass* cls,
               const std::function<bool(const DexMethod*)>& filter);

// Remove positions to make asserts easier to write with IRAssembler.
std::string stringify_for_comparision(DexMethod* method);
