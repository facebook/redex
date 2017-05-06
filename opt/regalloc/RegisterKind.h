/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "IRInstruction.h"
#include "Transform.h"

#include <unordered_map>

/*
 * This module was built with the intention of selecting the right move
 * instruction. In particular, we have a number of opcodes -- like if-* and
 * const opcodes -- that are capable of working with both object-bearing and
 * non-object registers, and if we were to insert moves for these registers, it
 * appeared that we needed to use dataflow analysis to figure out the
 * object-ness of the registers in order to pick the right opcode (move or
 * move-object).
 *
 * However, some cursory testing seems to indicate that the Dalvik / ART
 * verifier doesn't complain when we use the "wrong" move opcode.
 *
 * As it is, the module does an incomplete job of solving for register kinds,
 * because it only analyzes dataflow in the forward direction. This doesn't
 * cover cases like
 *
 *   const/4 v0 # could be used as either an object or int
 *   move v0, v1 # or move-object??
 *   invoke-direct LFoo;.<init>() v1 # now we know that {v0, v1} are objects
 *
 * I'm leaving the code in for now, since it helps us produce "less incorrect"
 * bytecode, and we can decide whether to fix it up / remove it entirely later
 * when we have a better idea of its usefulness.
 */

enum class RegisterKind {
  UNKNOWN, NORMAL, WIDE, OBJECT,
  // note that having a register of a MIXED kind is fine as long as we don't
  // read from it
  MIXED
};

class KindVec {
  std::vector<RegisterKind> m_vec;
 public:
  explicit KindVec(size_t n) { m_vec.resize(n); }
  RegisterKind& at(size_t i) { return m_vec.at(i); }
  RegisterKind at(size_t i) const { return m_vec.at(i); }
  RegisterKind& operator[](size_t i) {
    if (i >= m_vec.size()) {
      m_vec.resize(i + 1, RegisterKind::UNKNOWN);
    }
    return m_vec[i];
  }
  void meet(const KindVec&);
  bool operator==(const KindVec&) const;
  bool operator!=(const KindVec& that) const {
    return !(*this == that);
  }
};

std::string show(RegisterKind);

RegisterKind dest_kind(DexOpcode op);

std::unique_ptr<std::unordered_map<IRInstruction*, KindVec>>
analyze_register_kinds(IRCode*);
