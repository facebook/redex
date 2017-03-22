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
#include <string>
#include <vector>

/*
 * Stringification functions for core types.  Definitions are in DexClass.cpp
 * to avoid circular dependences.
 */
class DexString;
class DexType;
class DexField;
class DexTypeList;
class DexProto;
class DexCode;
class DexMethod;
class DexClass;
class DexEncodedValue;
class DexAnnotation;
class DexAnnotationSet;
class DexAnnotationDirectory;
class DexDebugInstruction;
class IRInstruction;
class MethodTransform;
class ControlFlowGraph;
struct MethodItemEntry;
struct DexDebugEntry;
struct DexPosition;
struct MethodCreator;
struct MethodBlock;
class Liveness;

std::string show(const DexString*);
std::string show(const DexType*);
std::string show(const DexField*);
std::string show(const DexDebugEntry*);
std::string show(const DexTypeList*);
std::string show(const DexProto*);
std::string show(const DexCode*);
std::string show(const DexMethod*);
std::string show(const DexPosition*);
std::string show(const DexClass*);
std::string show(const DexEncodedValue*);
std::string show(const DexAnnotation*);
std::string show(const DexAnnotationSet*);
std::string show(const DexAnnotationDirectory*);
std::string show(const DexDebugInstruction*);
std::string show(const IRInstruction*);
std::string show(const MethodTransform*);
std::string show(const MethodItemEntry&);
std::string show(const ControlFlowGraph&);
std::string show(const MethodCreator*);
std::string show(const MethodBlock*);
std::string show(const Liveness&);

template <typename T>
std::string show(const std::unique_ptr<T>& ptr) {
  return show(ptr.get());
}

// SHOW(x) is syntax sugar for show(x).c_str()
#define SHOW(...) show(__VA_ARGS__).c_str()

/**
 * Verbose show functions.
 * They print the given member in source language style including annotations.
 */
std::string vshow(const DexClass*);
std::string vshow(const DexMethod*);
std::string vshow(const DexField*);
