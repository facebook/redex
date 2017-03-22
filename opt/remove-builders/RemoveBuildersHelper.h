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

  void meet(const TaintedRegs& that);
  void trans(const IRInstruction*);

  bool operator==(const TaintedRegs& that) const;
  bool operator!=(const TaintedRegs& that) const;
};

/**
 * Using negative numbers here, since those will be used
 * alongside registers (uint16_t).
 */
enum FieldOrRegStatus : int {
  // Default mapping.
  DEFAULT = -1,

  // Field not initialized.
  UNDEFINED = -2,

  // Field initialized with different registers.
  DIFFERENT = -3,

  // Register that was storing the field's value was overwritten.
  OVERWRITTEN = -4,
};

struct FieldsRegs {
  std::unordered_map<DexField*, int> field_to_reg;

  explicit FieldsRegs(DexClass* builder) {
    const auto& ifields = builder->get_ifields();
    for (const auto& ifield : ifields) {
      field_to_reg[ifield] = FieldOrRegStatus::DEFAULT;
    }
  }
  explicit FieldsRegs(const std::unordered_map<DexField*, int>&& field_to_reg)
      : field_to_reg(std::move(field_to_reg)) {}

  void meet(const FieldsRegs& that);
  void trans(const IRInstruction*);

  bool operator==(const FieldsRegs& that) const;
  bool operator!=(const FieldsRegs& that) const;
};

/**
 * Returns the build method if one exists.
 */
DexMethod* get_build_method(const std::vector<DexMethod*>& vmethods);

/**
 * Given a method that calls the builder, it will remove it completely.
 */
bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee);

/**
 * Given a method and a builder, it inlines, if possible, the build method.
 */
bool inline_build(DexMethod* method, DexClass* builder);
