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

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "Inliner.h"
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

bool tainted_reg_escapes(
    DexType* type,
    DexMethod* method,
    const std::unordered_map<IRInstruction*, TaintedRegs>& taint_map,
    bool enable_buildee_constr_change = false);

void transfer_object_reach(DexType* object,
                           uint16_t regs_size,
                           const IRInstruction* insn,
                           RegSet& regs);

std::unique_ptr<std::unordered_map<IRInstruction*, TaintedRegs>>
get_tainted_regs(uint16_t regs_size,
                 const std::vector<cfg::Block*>& blocks,
                 DexType* type);

class BuilderTransform {
 public:
  BuilderTransform(const PassConfig& pc,
                   const Scope& scope,
                   DexStoresVector& stores,
                   bool throws_inline) {
    m_inliner_config.throws_inline = throws_inline;

    auto resolver = [&](DexMethodRef* method, MethodSearch search) {
      return resolve_method(method, search, m_resolved_refs);
    };

    std::unordered_set<DexMethod*> no_default_inlinables;
    m_inliner = std::unique_ptr<MultiMethodInliner>(new MultiMethodInliner(
        scope, stores, no_default_inlinables, resolver, m_inliner_config));
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

std::vector<DexMethod*> get_all_methods(IRCode* code, DexType* type);

std::vector<DexMethod*> get_non_init_methods(IRCode* code, DexType* type);

bool has_builder_name(DexType* cls);

/**
 * Given a builder, returns the enclosing class type.
 */
DexType* get_buildee(DexType* builder);

/**
 * Given a method and a builder, it will try to remove the builder completely.
 * - It expects that no builder methods are called and that the constructor
 *   doesn't call other super types constructors, except `Object` one.
 * - If super_class_holder is defined (!= nullptr), it will be used instead of
 *   the super_class
 */
bool remove_builder_from(DexMethod* method,
                         DexClass* builder,
                         BuilderTransform& b_transform,
                         DexType* super_class_holder = nullptr);
