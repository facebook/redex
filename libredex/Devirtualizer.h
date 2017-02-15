/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>


using ClassSet = std::unordered_set<DexClass*>;
using TypeSet = std::unordered_set<const DexType*>;

/*
 * DexType parent to children relationship
 * (child to parent is in DexClass)
 */
using ClassHierarchy = std::map<const DexType*, TypeSet>;

/**
 * Flags to mark virtual method state.
 * A combination of those flags and a SignatureMap should allow
 * faster analysis at virtual invocation site and across virtual
 * hierarchies.
 */
enum VirtualFlags : uint16_t {
  // the top method definition (DexMehthod) in a VirtualGroup
  TOP_DEF = 0x0,
  // the method is an override, it has a parent, it's a definition (DexMethod)
  OVERRIDE = 0X1,
  // the method contributes to an implementation of an interface
  // anywhere in any virtual hierarchy
  IMPL = 0X2,
  // the method is final, does not have any override, it's a leaf
  FINAL = 0X4,
};

inline VirtualFlags operator|=(VirtualFlags& a, const VirtualFlags b) {
  return (a = static_cast<VirtualFlags>(
      static_cast<uint16_t>(a) | static_cast<uint16_t>(b)));
}

inline VirtualFlags operator&=(VirtualFlags& a, const VirtualFlags b) {
  return (a = static_cast<VirtualFlags>(
      static_cast<uint16_t>(a) & static_cast<uint16_t>(b)));
}

inline VirtualFlags operator|(const VirtualFlags a, const VirtualFlags b) {
  return static_cast<VirtualFlags>(
      static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}

inline VirtualFlags operator&(const VirtualFlags a, const VirtualFlags b) {
  return static_cast<VirtualFlags>(
      static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}

// (DexMethod, VirtualFlags)
// Information on the flags of that method in relationship to
// the VirtualGroup.
using MethodFlags = std::pair<DexMethod*, VirtualFlags>;

// Vector of all methods in a virtual call hierarchy.
// for example, for the signature
// equals(java.lang.Object)
// you are guaranteed to find the VirtualGroup starting from
// Object.equals(Object) which includes all overrides in any class
using VirtualGroup = std::vector<MethodFlags>;

// map from a proto to a list of (DexMethods, VirtualFlags) with that proto
using ProtoMap = std::unordered_map<DexProto*, VirtualGroup>;

// map from a name to a map of proto with that name
// so SignatureMap will have something like the following:
// "meth1" -> | ()V     -> LA;.meth1()V, LB;.meth1()V
//            | (II)LA; -> LC;.meth1(II)LA;, LB;.meth1(II)LA;
// "meth2" -> | ()V     -> LB;.meth2()V, LE;.meth2()V
using SignatureMap = std::unordered_map<DexString*, ProtoMap>;


using ProtoSet = std::unordered_set<DexProto*>;

// a map from name to signatures for a set of interfaces
using InterfaceSigMap = std::unordered_map<DexString*, ProtoSet>;

ClassHierarchy build_type_hierarchy(const Scope& scope);
SignatureMap build_signature_map(const ClassHierarchy& class_hierarchy);

//
// Helpers
//

/**
 * Given a scope find all virtual methods that can be devirtualized.
 * That is, methods that have a unique definition in the vmethods across
 * a hierarchy. Basically all methods that are virtual because of visibility
 * (public, package and protected) and not because they need to be virtual.
 */
inline std::vector<DexMethod*> devirtualize(const SignatureMap& sig_map) {
  std::vector<DexMethod*> non_virtual;
  for (const auto& meths_by_name : sig_map) {
    for (const auto& meths_by_sig : meths_by_name.second) {
      for (const auto& meths : meths_by_sig.second) {
        if (!meths.first->is_concrete()) continue;
        if (meths.second == FINAL) non_virtual.push_back(meths.first);
      }
    }
  }
  return non_virtual;
}

inline std::vector<DexMethod*> devirtualize(
    const std::vector<DexClass*>& scope) {
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  auto signature_map = build_signature_map(class_hierarchy);
  return devirtualize(signature_map);
}

/**
 * Return the list of virtual methods for a given type.
 * If the type is java.lang.Object and it is not known (no DexClass for it)
 * it generates fictional methods for it.
 */
const std::vector<DexMethod*>& get_vmethods(const DexType* type);
