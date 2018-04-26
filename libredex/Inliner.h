/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "DexClass.h"
#include "DexStore.h"
#include "IRCode.h"
#include "Resolver.h"

namespace inliner {

/*
 * Inline tail-called `callee` into `caller` at `pos`.
 *
 * NB: This is NOT a general-purpose inliner; it assumes that the caller does
 * not do any work after the call, so the only live registers are the
 * parameters to the callee. This allows it to do inlining by simply renaming
 * the callee's registers. The more general inline_method instead inserts
 * move instructions to map the caller's argument registers to the callee's
 * params.
 *
 * In general, use of this method should be considered deprecated. It is
 * currently only being used by the BridgePass because the insertion of
 * additional move instructions would confuse SynthPass, which looks for
 * exact sequences of instructions.
 */
void inline_tail_call(DexMethod* caller,
                      DexMethod* callee,
                      IRList::iterator pos);

/*
 * Inline `callee` into `caller` at `pos`.
 * This is a general-purpose inliner.
 */
void inline_method(IRCode* caller,
                   IRCode* callee,
                   IRList::iterator pos);

} // namespace inliner

/**
 * Helper class to inline a set of candidates.
 * Take a set of candidates and a scope and walk all instructions in scope
 * to find and inline all calls to candidate.
 * A resolver is used to map a method reference to a method definition.
 * Not all methods may be inlined both for restriction on the caller or the
 * callee.
 * Perform inlining bottom up.
 */
class MultiMethodInliner {
 public:
  struct Config {
    bool throws_inline;
    bool enforce_method_size_limit{true};
    std::unordered_set<DexType*> black_list;
    std::unordered_set<DexType*> caller_black_list;
    std::unordered_set<DexType*> whitelist_no_method_limit;
  };

  MultiMethodInliner(
      const std::vector<DexClass*>& scope,
      DexStoresVector& stores,
      const std::unordered_set<DexMethod*>& candidates,
      std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolver,
      const Config& config);

  ~MultiMethodInliner() {
    invoke_direct_to_static();
  }

  /**
   * attempt inlining for all candidates.
   */
  void inline_methods();

  /**
   * Return the count of unique inlined methods.
   */
  std::unordered_set<DexMethod*> get_inlined() const {
    return inlined;
  }

  /**
   * Inline callees in the caller if is_inlinable below returns true.
   */
  void inline_callees(DexMethod* caller,
                      const std::vector<DexMethod*>& callees);

  /**
   * Inline callees in the given instructions in the caller, if is_inlinable
   * below returns true.
   */
  void inline_callees(DexMethod* caller,
                      const std::unordered_set<IRInstruction*>& insns);

 private:
  /**
   * Inline all callees into caller.
   * Recurse in a callee if that has inlinable candidates of its own.
   * Inlining is bottom up.
   */
  void caller_inline(
      DexMethod* caller,
      const std::vector<DexMethod*>& callees,
      std::unordered_set<DexMethod*>& visited);

  /**
   * Return true if the callee is inlinable into the caller.
   * The predicates below define the constraint for inlining.
   */
  bool is_inlinable(const DexMethod* caller,
                    const DexMethod* callee,
                    size_t estimated_insn_size);


  void inline_inlinables(
      DexMethod* caller,
      const std::vector<std::pair<DexMethod*, IRList::iterator>>& inlinables);

  /**
   * Return true if the method is related to enum (java.lang.Enum and derived).
   * Cannot inline enum methods because they can be called by code we do
   * not own.
   */
  bool is_blacklisted(const DexMethod* callee);

  bool caller_is_blacklisted(const DexMethod* caller);

  /**
   * Return true if the callee contains external catch exception types
   * which are not public.
   */
  bool has_external_catch(const DexMethod* callee);

  /**
   * Return true if the callee contains certain opcodes that are difficult
   * or impossible to inline.
   * Some of the opcodes are defined by the methods below.
   */
  bool cannot_inline_opcodes(const DexMethod* caller, const DexMethod* callee);

  /**
   * Return true if inlining would require a method called from the callee
   * (candidate) to turn into a virtual method (e.g. private to public).
   */
  bool create_vmethod(IRInstruction* insn);

  /**
   * Return true if a callee contains an invoke super to a different method
   * in the hierarchy, and the callee and caller are in different classes.
   * invoke-super can only exist within the class the call lives in.
   */
  bool nonrelocatable_invoke_super(IRInstruction* insn,
                                   const DexMethod* callee,
                                   const DexMethod* caller);

  /**
   * Return true if a callee overrides one of the input registers.
   * Writing over an input registers may change the type of the registers
   * in the caller if the method was inlined and break invariants in the caller.
   */
  bool writes_ins_reg(IRInstruction* insn, uint16_t temp_regs);

  /**
   * Return true if the callee contains a call to an unknown virtual method.
   * We cannot determine the visibility of the method invoked and thus
   * we cannot inline as we could cause a verification error if the method
   * was package/protected and we move the call out of context.
   */
  bool unknown_virtual(IRInstruction* insn,
                       const DexMethod* caller,
                       const DexMethod* callee);

  /**
   * Return true if the callee contains a call to an unknown field.
   * We cannot determine the visibility of the field accessed and thus
   * we cannot inline as we could cause a verification error if the field
   * was package/protected and we move the access out of context.
   */
  bool unknown_field(IRInstruction* insn,
                     const DexMethod* callee,
                     const DexMethod* caller);

  /**
   * Return true if a caller is in a DEX in a store and any opcode in callee
   * refers to a DexMember in a different store .
   */
  bool cross_store_reference(const DexMethod* context);

  bool is_estimate_over_max(uint64_t estimated_insn_size,
                            const DexMethod* callee,
                            uint64_t max);

  /**
   * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
   * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
   *
   * Right now we only check for the number of instructions, but there are
   * other constraints that might be worth looking into, e.g. the number of
   * registers.
   */
  bool caller_too_large(DexType* caller_type,
                        size_t estimated_caller_size,
                        const DexMethod* callee);

  /**
   * Staticize required methods (stored in `m_make_static`) and update
   * opcodes accordingly.
   *
   * NOTE: It only needs to be called once after inlining. Since it is called
   *       from the destructor, there is no need to manually call it.
   */
  void invoke_direct_to_static();

 private:
  /**
   * Resolver function to map a method reference to a method definition.
   */
  std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolver;

  /**
   * Checker for cross stores contaminations.
   */
  XStoreRefs xstores;

  /**
   * Inlined methods.
   */
  std::unordered_set<DexMethod*> inlined;

  //
  // Maps from callee to callers and reverse map from caller to callees.
  // Those are used to perform bottom up inlining.
  //
  std::unordered_map<DexMethod*, std::vector<DexMethod*>> callee_caller;
  // this map is ordered in order that we inline our methods in a repeatable
  // fashion so as to create reproducible binaries
  std::map<DexMethod*, std::vector<DexMethod*>, dexmethods_comparator>
      caller_callee;

 private:
  /**
   * Info about inlining.
   */
  struct InliningInfo {
    size_t calls_inlined{0};
    size_t recursive{0};
    size_t not_found{0};
    size_t blacklisted{0};
    size_t throws{0};
    size_t multi_ret{0};
    size_t need_vmethod{0};
    size_t invoke_super{0};
    size_t write_over_ins{0};
    size_t escaped_virtual{0};
    size_t non_pub_virtual{0};
    size_t escaped_field{0};
    size_t non_pub_field{0};
    size_t non_pub_ctor{0};
    size_t cross_store{0};
    size_t caller_too_large{0};
  };
  InliningInfo info;

  const std::vector<DexClass*>& m_scope;

  const Config& m_config;

  std::unordered_set<DexMethod*> m_make_static;

 public:
  const InliningInfo& get_info() {
    return info;
  }
};

/**
 * Add the single-callsite methods to the inlinable set.
 */
void select_inlinable(
    const Scope& scope,
    const std::unordered_set<DexMethod*>& methods,
    MethodRefCache& resolved_refs,
    std::unordered_set<DexMethod*>* inlinable,
    bool multiple_callee = false);
