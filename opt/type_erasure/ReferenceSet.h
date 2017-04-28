/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

/**
* A reference. Holds both the instruction containing the
* reference and the method the ref lives in.
*/
struct InstRef {
  const DexMethod* method;
  const IRInstruction* insn;

  InstRef(const DexMethod* method, const IRInstruction* insn) :
     method(method), insn(insn) {}

  bool is_type_ref() const {
   return insn->has_type();
  }

  bool is_method_ref() const {
   return insn->has_method();
  }
};

using FieldRefs = std::unordered_map<const DexType*, std::vector<DexField*>>;
using SigRefs = std::unordered_map<const DexType*, std::vector<DexProto*>>;
using CodeRefs = std::unordered_map<const DexType*, std::vector<InstRef>>;

struct ReferenceSet {
  TypeSet all_refs;
  FieldRefs field_refs;
  SigRefs sig_refs;
  CodeRefs code_refs;
  TypeSet unrfs;

  ReferenceSet(const Scope& scope, const TypeSet& ref_set);
  void print() const;
};
