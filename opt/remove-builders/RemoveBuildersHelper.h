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
#include "IRInstruction.h"
#include "InlineHelper.h"
#include "Resolver.h"

using RegSet = boost::dynamic_bitset<>;

struct TaintedRegs {
  RegSet m_reg_set;

  explicit TaintedRegs(int nregs) : m_reg_set(nregs) {}
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
  std::unordered_map<DexField*, std::unordered_set<const IRInstruction*>>
      field_to_iput_insns;

  explicit FieldsRegs(DexClass* builder) {
    const auto& ifields = builder->get_ifields();
    for (const auto& ifield : ifields) {
      field_to_reg[ifield] = FieldOrRegStatus::DEFAULT;
      field_to_iput_insns[ifield] = std::unordered_set<const IRInstruction*>();
    }
  }

  void meet(const FieldsRegs& that);
  void trans(const IRInstruction*);

  bool operator==(const FieldsRegs& that) const;
  bool operator!=(const FieldsRegs& that) const;
};

/**
 * Given a method that calls the builder, it will remove it completely.
 */
bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee);

bool has_builder_name(DexClass* cls);

DexType* get_buildee(DexType* type);

class BuilderTransform {
 public:
  BuilderTransform(const PassConfig& pc,
                   const Scope& scope,
                   const DexClasses& primary_dex) {
    m_inliner_config.callee_direct_invoke_inline = true;
    m_inliner_config.virtual_same_class_inline = true;
    m_inliner_config.super_same_class_inline = true;
    m_inliner_config.use_liveness = true;
    m_inliner_config.no_exceed_16regs = true;

    auto resolver = [&](DexMethod* method, MethodSearch search) {
      return resolve_method(method, search, m_resolved_refs);
    };

    std::unordered_set<DexMethod*> no_default_inlinables;
    m_inliner = std::unique_ptr<MultiMethodInliner>(new MultiMethodInliner(
        scope, primary_dex, no_default_inlinables, resolver, m_inliner_config));
  }

  bool inline_methods(DexMethod* method,
                      DexType* type,
                      std::function<std::vector<DexMethod*>(IRCode*, DexType*)>
                          get_methods_to_inline);

 private:
  std::unique_ptr<MultiMethodInliner> m_inliner;
  MultiMethodInliner::Config m_inliner_config;
  MethodRefCache m_resolved_refs;
};
