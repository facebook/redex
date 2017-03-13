/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/dynamic_bitset.hpp>

#include "DexClass.h"

using RegSet = boost::dynamic_bitset<>;

struct TaintedRegs {
  RegSet m_reg_set;

  explicit TaintedRegs(int nregs): m_reg_set(nregs) {}
  explicit TaintedRegs(const RegSet&& reg_set)
      : m_reg_set(std::move(reg_set)) {}

  const RegSet& bits() { return m_reg_set; }

  void meet(const TaintedRegs& that) {
    m_reg_set |= that.m_reg_set;
  }
  void trans(const DexInstruction*);
  bool operator==(const TaintedRegs& that) const {
    return m_reg_set == that.m_reg_set;
  }
  bool operator!=(const TaintedRegs& that) const {
    return !(*this == that);
  }
};

/**
 * Returns the build method if one exists.
 */
DexMethod* get_build_method(std::vector<DexMethod*>& vmethods);

/**
 * Given a method that calls the builder, it will remove it completely.
 */
bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee);

/**
 * Given a method and a builder, it inlines, if possible, the build method.
 */
bool inline_build(DexMethod* method, DexClass* builder);
