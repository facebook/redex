/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "DexClass.h"
#include "IRInstruction.h"
#include "SingleImpl.h"
#include "ClassHierarchy.h"

/**
 * Analyze and optimize data structures.
 * The data set is filled during analysis and it's used by the optimizer.
 */

using Scope = std::vector<DexClass*>;

using TypeList = std::vector<DexType*>;
using TypeMap = std::unordered_map<DexType*, DexType*>;
using TypeToTypes = std::unordered_map<DexType*, TypeList>;
using FieldList = std::vector<DexField*>;
using OrderedMethodSet = std::set<DexMethod*, dexmethods_comparator>;
using OpcodeList = std::vector<IRInstruction*>;
using OpcodeSet = std::unordered_set<IRInstruction*>;
using FieldRefToOpcodes = std::unordered_map<DexFieldRef*, OpcodeList>;
using MethodToOpcodes = std::unordered_map<DexMethodRef*, OpcodeSet>;
using NewMethods = std::unordered_map<DexMethodRef*, DexMethodRef*>;
using NewVTable = std::vector<std::pair<DexMethod*, DexMethod*>>;

/**
 * Possible escape reason of interfaces.
 * Those are all problematic cases that require us to drop the optimization
 * or require deeper analysis.
 */
enum EscapeReason : uint32_t {
  NO_ESCAPE = 0x0,
  // analysis escape reason
  // inteface has a <clinit>
  CLINIT = 0x1,
  // interface has static fields
  HAS_SFIELDS = 0x2,
  // interface appears in an array type
  HAS_ARRAY_TYPE = 0x4,
  // interface is in the signature of a native method
  NATIVE_METHOD = 0x8,
  // a method ref to the interface is for a method not defined on the interface
  UNKNOWN_MREF = 0x20,
  // a field ref whose class is the interface
  HAS_FIELD_REF = 0X40,
  // filtered by config
  FILTERED = 0x80,
  // parent is unknown to redex
  IMPL_PARENT_ESCAPED = 0x100,
  // interface marked DoNotStrip
  DO_NOT_STRIP = 0X400,
  // create a reference across stores that is illegal
  CROSS_STORES = 0x800,
  // optimization escape reason
  // interface substitution causes a collision with an existing method
  SIG_COLLISION = 0x10000,
  // interface substitution causes a collision with an existing field
  FIELD_COLLISION = 0x20000,
  // move the interface to the next pass. Something dropped the interface
  // for the current pass
  NEXT_PASS = 0x40000,
};
inline EscapeReason operator|=(EscapeReason& a, const EscapeReason b) {
  return (a = static_cast<EscapeReason>(static_cast<uint32_t>(a) |
                                        static_cast<uint32_t>(b)));
}
inline EscapeReason operator|(const EscapeReason a, const EscapeReason b) {
  return static_cast<EscapeReason>(static_cast<uint32_t>(a) |
                                   static_cast<uint32_t>(b));
}
inline EscapeReason operator&(const EscapeReason a, const EscapeReason b) {
  return static_cast<EscapeReason>(static_cast<uint32_t>(a) &
                                   static_cast<uint32_t>(b));
}

/**
 * For every single implemented interface the set of data related to that
 * interface only.
 * Every map here points to the original (as found in analysis) def/ref.
 * Fielddef/ref and typeref are easy to manage in that an optimization step
 * (if allowed) can simply go through and flip the type.
 * Methods handling is more complex as each method may have multiple
 * interfaces in the signature. The optimizer keeps track of current
 * methods as they get rewritten.
 */
struct SingleImplData {
  // single concrete class for the single impl interface (entry in the
  // SingleImplInterfaces map)
  DexType* cls;
  // Single impl interface escape
  EscapeReason escape;
  // Direct children of the interface
  TypeSet children;
  // single impl interface typed fields
  FieldList fielddefs;
  // methods with the single impl interface in the signature
  OrderedMethodSet methoddefs;
  // single impl interface typerefs
  OpcodeList typerefs;
  // single impl interface typed fieldref opcode
  FieldRefToOpcodes fieldrefs;
  // invoke-interface to the single impl interface methods
  MethodToOpcodes intf_methodrefs;
  // opcodes to a methodref with the single impl interface in the signature
  MethodToOpcodes methodrefs;

  bool is_escaped() const { return escape != NO_ESCAPE; }
};

// map from single implemented interfaces to the data related to that interface
using SingleImpls = std::unordered_map<DexType*, SingleImplData>;

struct SingleImplAnalysis {
  /**
   * Create a SingleImplAnalysis from a given Scope.
   */
  static std::unique_ptr<SingleImplAnalysis> analyze(
      const Scope& scope, const DexStoresVector& stores,
      const TypeMap& single_impl, const TypeSet& intfs,
      const ProguardMap& pg_map,
      const SingleImplConfig& config);

  /**
   * Escape an interface and all parent interfaces.
   */
  void escape_interface(DexType* intf, EscapeReason reason);

  /**
   * Return whether a type is escaped. Works with any type.
   */
  bool is_escaped(DexType* intf) const {
    auto single_impl = single_impls.find(intf);
    return single_impl != single_impls.end() &&
           single_impl->second.is_escaped();
  }

  /**
   * Return whether a type is single impl.
   */
  bool is_single_impl(DexType* intf) const {
    auto single_impl = single_impls.find(intf);
    return single_impl != single_impls.end();
  }

  /**
   * Load the list of interface to optimize.
   */
  void get_interfaces(TypeList& to_optimize) const;

  SingleImplData& get_single_impl_data(DexType* intf) {
    return single_impls.at(intf);
  }

 protected:
  SingleImpls single_impls;
};

/**
 * Run an optimization pass over a SingleImplAnalysis.
 */
size_t optimize(
    std::unique_ptr<SingleImplAnalysis> analysis,
    const ClassHierarchy& ch,
    Scope& scope,
    const SingleImplConfig& config);
