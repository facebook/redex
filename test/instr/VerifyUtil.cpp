/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <gtest/gtest.h>

#include "VerifyUtil.h"

DexClass* find_class_named(const DexClasses& classes, const char* name) {
  auto it = std::find_if(classes.begin(), classes.end(), [&name](DexClass* cls){
    return !strcmp(name, cls->get_name()->c_str());
  });
  return it == classes.end() ? nullptr : *it;
}

DexMethod* find_vmethod_named(const DexClass& cls, const char* name) {
  auto vmethods = cls.get_vmethods();
  auto it = std::find_if(vmethods.begin(), vmethods.end(), [&name](DexMethod* m){
    return !strcmp(name, m->get_name()->c_str());
  });
  return it == vmethods.end() ? nullptr : *it;
}

DexInstruction* find_instruction(const DexMethod* m, uint32_t opcode) {
  for (auto& insn : m->get_code()->get_instructions()) {
    if (insn->opcode() == opcode) {
      return insn;
    }
  }
  return nullptr;
}
