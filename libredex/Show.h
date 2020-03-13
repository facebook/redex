/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <set>
#include <sstream>
#include <string>

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
class DexCallSite;
class DexMethodHandle;

namespace cfg {
class Block;
class ControlFlowGraph;
} // namespace cfg

struct MethodItemEntry;
struct DexDebugEntry;
struct DexPosition;
struct MethodCreator;
struct MethodBlock;
namespace ir_list {
template <bool is_const>
class InstructionIterableImpl;

using InstructionIterable = InstructionIterableImpl<false>;
} // namespace ir_list
using SwitchIndices = std::set<int>;

/*
 * If an object has the << operator defined, use that to obtain its string
 * representation. But make sure we don't print pointer addresses.
 */
template <typename T,
          // Use SFINAE to check for the existence of operator<<
          typename = decltype(std::declval<std::ostream&>()
                              << std::declval<T>()),
          typename = std::enable_if_t<!std::is_pointer<std::decay_t<T>>::value>>
std::string show(T&& t) {
  std::ostringstream o;
  o << std::forward<T>(t);
  return o.str();
}

/*
 * If we have a pointer, try to obtain the string representation of the value
 * it points to.
 */
template <typename T,
          // Use SFINAE to check for the existence of operator<<
          typename = decltype(std::declval<std::ostream&>()
                              << std::declval<T>())>
std::string show(T* t) {
  std::ostringstream o;
  if (t != nullptr) {
    o << *t;
  }
  return o.str();
}

template <typename T>
std::string show(const std::unique_ptr<T>& ptr) {
  return show(ptr.get());
}

// XXX Currently, we have some printing methods defined as operator<< and
// others as show(). I (jezng) would like to see us standardize on operator<<
// if possible. The template methods above provide a bridge in the meantime,
// allowing us to use show() whenever operator<< is defined.
std::ostream& operator<<(std::ostream&, const DexString&);
std::ostream& operator<<(std::ostream&, const DexType&);
std::ostream& operator<<(std::ostream&, const DexClass&);
std::ostream& operator<<(std::ostream&, const DexPosition&);
std::ostream& operator<<(std::ostream&, const DexFieldRef&);
std::ostream& operator<<(std::ostream&, const IRInstruction&);
std::ostream& operator<<(std::ostream&, const MethodItemEntry&);
std::ostream& operator<<(std::ostream&, const DexCallSite&);
std::ostream& operator<<(std::ostream&, const DexMethodHandle&);

std::string show(const DexFieldRef*);
std::string show(const DexDebugEntry*);
std::string show(const DexTypeList*);
std::string show(const DexProto*);
std::string show(const DexCode*);
std::string show(const DexMethodRef*);
std::string show(const DexEncodedValue*);
std::string show(const DexAnnotation*);
std::string show(const DexAnnotationSet*);
std::string show(const DexAnnotationDirectory*);
std::string show(const DexDebugInstruction*);
std::string show(const IRInstruction*);
std::string show(const IRCode*);
std::string show(const cfg::Block* block);
std::string show(const cfg::ControlFlowGraph&);
std::string show(const MethodCreator*);
std::string show(const MethodBlock*);
std::string show(const ir_list::InstructionIterable&);
std::string show(const SwitchIndices& si);

// Variants of show that use deobfuscated names
std::string show_deobfuscated(const DexType* t);
std::string show_deobfuscated(const DexClass*);
std::string show_deobfuscated(const DexAnnotation*);
std::string show_deobfuscated(const DexFieldRef*);
std::string show_deobfuscated(const DexMethodRef*);
std::string show_deobfuscated(const IRInstruction*);
std::string show_deobfuscated(const DexEncodedValue*);
std::string show_deobfuscated(const DexTypeList*);
std::string show_deobfuscated(const DexProto*);
std::string show_deobfuscated(const DexCallSite*);
std::string show_deobfuscated(const DexMethodHandle*);

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
std::string vshow(uint32_t acc, bool is_method = true); // DexAccessFlags
std::string vshow(const DexType*);

// Format a number as a byte entity.
std::string pretty_bytes(uint64_t val);
