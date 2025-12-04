/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/dynamic_bitset.hpp>
#include <utility>

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "Resolver.h"

using RegSet = boost::dynamic_bitset<>;

struct TaintedRegs {
  RegSet m_reg_set;

  TaintedRegs() : m_reg_set(0) {}
  explicit TaintedRegs(int nregs) : m_reg_set(nregs) {}
  explicit TaintedRegs(RegSet&& reg_set) noexcept
      : m_reg_set(std::move(reg_set)) {}

  const RegSet& bits() { return m_reg_set; }

  void meet(const TaintedRegs& that);
  void trans(const IRInstruction*);

  bool operator==(const TaintedRegs& that) const;
  bool operator!=(const TaintedRegs& that) const;
};

/**
 * Using negative numbers here, since those will be used
 * alongside registers (uint32_t).
 */
enum FieldOrRegStatus : int64_t {
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
  UnorderedMap<DexField*, int> field_to_reg;
  UnorderedMap<DexField*, UnorderedSet<const IRInstruction*>>
      field_to_iput_insns;

  FieldsRegs() {}
  explicit FieldsRegs(DexClass* builder) {
    const auto& ifields = builder->get_ifields();
    for (const auto& ifield : ifields) {
      field_to_reg[ifield] = FieldOrRegStatus::DEFAULT;
      field_to_iput_insns[ifield] = UnorderedSet<const IRInstruction*>();
    }
  }

  void meet(const FieldsRegs& that);
  void trans(const IRInstruction*);

  bool operator==(const FieldsRegs& that) const;
  bool operator!=(const FieldsRegs& that) const;
};

bool tainted_reg_escapes(
    const DexType* type,
    DexMethod* method,
    const UnorderedMap<IRInstruction*, TaintedRegs>& taint_map,
    bool enable_buildee_constr_change = false);

void transfer_object_reach(const DexType* object,
                           uint32_t regs_size,
                           const IRInstruction* insn,
                           RegSet& regs);

std::unique_ptr<UnorderedMap<IRInstruction*, TaintedRegs>> get_tainted_regs(
    const cfg::ControlFlowGraph& cfg,
    uint32_t regs_size,
    const std::vector<cfg::Block*>& blocks,
    const DexType* type);

class BuilderTransform {
 public:
  BuilderTransform(const init_classes::InitClassesWithSideEffects&
                       init_classes_with_side_effects,
                   inliner::InlinerConfig inliner_config,
                   const Scope& scope,
                   DexStoresVector& stores,
                   const ConfigFiles& conf,
                   bool throws_inline)
      : m_inliner_config(std::move(inliner_config)) {
    // Note: We copy global inline config in the class since it seems that it
    // may be configured differently from global inliner_config.throws_inline.
    // Maybe we can refactor this part.
    m_inliner_config.throws_inline = throws_inline;

    UnorderedSet<DexMethod*> no_default_inlinables;
    int min_sdk = 0;
    m_inliner = std::unique_ptr<MultiMethodInliner>(new MultiMethodInliner(
        scope, init_classes_with_side_effects, stores, conf,
        no_default_inlinables, std::ref(m_concurrent_method_resolver),
        m_inliner_config, min_sdk));
  }

  bool inline_methods(
      DexMethod* method,
      const DexType* type,
      const std::function<UnorderedSet<DexMethod*>(IRCode*, const DexType*)>&
          get_methods_to_inline);

  void flush() { m_inliner->flush(); }

 private:
  std::unique_ptr<MultiMethodInliner> m_inliner;
  inliner::InlinerConfig m_inliner_config;
  ConcurrentMethodResolver m_concurrent_method_resolver;
};

UnorderedSet<DexMethod*> get_all_methods(IRCode* code, const DexType* type);

UnorderedSet<DexMethod*> get_non_init_methods(IRCode* code,
                                              const DexType* type);

bool has_builder_name(const DexType* type);

/**
 * Given a builder, returns the enclosing class type.
 */
DexType* get_buildee(const DexType* builder);

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
