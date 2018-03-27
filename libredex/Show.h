/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

/*
 * Stringification functions for core types.  Definitions are in DexClass.cpp
 * to avoid circular dependences.
 */
class DexString;
class DexType;
class DexFieldRef;
class DexField;
class DexTypeList;
class DexProto;
class DexCode;
class DexMethodRef;
class DexMethod;
class DexClass;
class DexEncodedValue;
class DexAnnotation;
class DexAnnotationSet;
class DexAnnotationDirectory;
class DexDebugInstruction;
class IRInstruction;
class IRCode;
class ControlFlowGraph;
struct MethodItemEntry;
struct DexDebugEntry;
struct DexPosition;
struct MethodCreator;
struct MethodBlock;
namespace ir_list {
  template <bool is_const>
  class InstructionIterableImpl;

  using InstructionIterable = InstructionIterableImpl<false>;
}
using SwitchIndices = std::set<int>;

std::string show(const DexString*);
std::string show(const DexType*);
std::string show(const DexFieldRef*);
std::string show(const DexDebugEntry*);
std::string show(const DexTypeList*);
std::string show(const DexProto*);
std::string show(const DexCode*);
std::string show(const DexMethodRef*);
std::string show(const DexPosition*);
std::string show(const DexClass*);
std::string show(const DexEncodedValue*);
std::string show(const DexAnnotation*);
std::string show(const DexAnnotationSet*);
std::string show(const DexAnnotationDirectory*);
std::string show(const DexDebugInstruction*);
std::string show(const IRInstruction*);
std::string show(const IRCode*);
std::string show(const MethodItemEntry&);
std::string show(const ControlFlowGraph&);
std::string show(const MethodCreator*);
std::string show(const MethodBlock*);
std::string show(const ir_list::InstructionIterable&);
std::string show(const SwitchIndices& si);

// Variants of show that use deobfuscated names
std::string show_deobfuscated(const DexClass*);
std::string show_deobfuscated(const DexAnnotation*);
std::string show_deobfuscated(const DexFieldRef*);
std::string show_deobfuscated(const DexMethodRef*);
std::string show_deobfuscated(const IRInstruction*);
std::string show_deobfuscated(const DexEncodedValue*);

template <typename T>
std::string show(const std::unique_ptr<T>& ptr) {
  return show(ptr.get());
}

// SHOW(x) is syntax sugar for show(x).c_str()
#define SHOW(...) show(__VA_ARGS__).c_str()

std::string show_context(IRCode const*, IRInstruction const*);
#define SHOW_CONTEXT(code, insn) (show_context(code, insn).c_str())

/**
 * Verbose show functions.
 * They print the given member in source language style including annotations.
 */
std::string vshow(const DexClass*);
std::string vshow(const DexMethod*, bool include_annotations = true);
std::string vshow(const DexField*);
std::string vshow(uint32_t acc); // for modifiers
std::string vshow(const DexType*);
