/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "Timer.h"
#include <vector>
#include <map>
#include <set>


using TypeSet = std::set<const DexType*>;

/**
 * DexType parent to children relationship
 * (child to parent is in DexClass)
 */
using ClassHierarchy = std::map<const DexType*, TypeSet>;

/**
 * Flags to mark virtual method state.
 * A combination of DexMethod::get_access(), those flags
 * and a SignatureMap should have a lot of what you need to
 * make decisions on how to operate on a method.
 * Ex:
 * VirtualFlags flags = ...;
 * flags == (see below)
 * TOP_DEF: first definition in the hierarchy, so where the
 *    method (virtual scope) was introduced
 * OVERRIDE: a child of a TOP_DEF
 * OVERRIDE | FINAL: a leaf method
 * TOP_DEF | FINAL: a method that is virtual only because
 *    of visibility but could be made static
 * IMPL | <one of the above>: the method contributes (lexically) to
 *    interface resolution
 * MIRANDA | <above>: the method is an implementation of an interface at the
 *    class with "implements". MIRANDA are created if they are not there and so
 *    they may or may not be concrete (is_concrete()). A class is
 *    guaranteed to have every 'implemented interface scope' rooted
 *    to a MIRANDA
 * ESCAPED | <above>: bad luck. Somewhere an interface could not be resolved
 *    and so we cannot tell anything about all methods in the branch where that
 *    happened. The method is effectively unknown
 */
enum VirtualFlags : uint16_t {
  // the top method definition (DexMehthod) in a VirtualScope.
  // This is where the method was first introduced for the virtual scope.
  TOP_DEF =         0x0,
  // the method is an override, it has a parent
  OVERRIDE =        0X1,
  // the method contributes to an implementation of an interface
  IMPL =            0X2,
  // the method is final, does not have any override, it's a leaf
  FINAL =           0X4,
  // the method is an implementation of an interface at the point
  // where the interface is defined. Effectively at the 'implements' class
  MIRANDA =         0x8,
  // the method may escape context/scope. This happens when a class
  // implements an unknown interface in which case the entir branch
  // up to object will have to escape
  ESCAPED =         0x100,
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
// VirtualFlags of that method in relationship to the VirtualScope.
using VirtualMethod = std::pair<DexMethod*, VirtualFlags>;

/**
 * A VirtualScope is defined by:
 * - type: the type or interface the scope is for
 * - methods: the list of all the methods for that scope.
 *            The method at position 0 is the top method for that
 *            virtual scope. The others are "randomly" ordered.
 * - interfaces: the set of interfaces that scope honors.
 *               This set is only present for class scopes, not for interfaces
 * For example, for the signature
 * equals(java.lang.Object)
 * you are guaranteed to find the VirtualScope starting from
 * Object.equals(Object) which includes all overrides in any class
 * and possibly any interface that exposes bool equals(Object)
 */
struct VirtualScope {
  const DexType* type;
  std::vector<VirtualMethod> methods;
  TypeSet interfaces;
};

/**
 * Return true if a VirtualScope can be renamed.
 */
bool can_rename_scope(const VirtualScope* scope);

/**
 * Return true if a VirtualScope contributes to interface resolution.
 */
bool is_impl_scope(const VirtualScope* scope);

/**
 * A SignatureMap is the following
 * { DexString* (virtual method name) ->
 *        { DexProto* (sig) ->
 *                  [VirtualScope, ..., VirtualScope],
 *                  ....
 *        },
 *        ....
 * }
 * A SignatureMap is build via a walk through the "lexical virtual scope"
 * building the sets of methods that are in the same scope.
 * In the process a set of useful flags are computed and stored.
 * So, in
 * class A { void m(); }
 * the SignatureMap returned, on top of the Object entries, will have
 * { Object virtual scope,
 *   "m" -> { void() -> [ VirtualScope{A, [(A.m(), TOP_DEF | FINAL)], {}}]}
 * }
 * if we add to the scope
 * class B { void m(); void f(); }
 * the resulting map would be
 * { Object virtual scope,
 *   "m" -> { void() -> [
                VirtualScope{A, [(A.m(), TOP_DEF | FINAL)], {}},
                VirtualScope{B, [(B.m(), TOP_DEF | FINAL)], {}}]}
 *   "f" -> { void() -> [ VirtualScope{B, [(B.f(), TOP_DEF | FINAL)], {}}]}
 * }
 * and adding
 * class C extends A { void m(); }
 * would give
 * { Object virtual scope,
 *   "m" -> { void() -> [
                VirtualScope{A, [
                        (A.m(), TOP_DEF),
                        (C.m(), OVERRIDE | FINAL)], {}},
                VirtualScope{B, [(B.m(), TOP_DEF | FINAL)], {}}]}
 *   "f" -> { void() -> [ VirtualScope{B, [(B.f(), TOP_DEF | FINAL)], {}}]}
 * }
 * interface add a funny spin to this as can be explored in the unit tests.
 */
using VirtualScopes = std::vector<VirtualScope>;
// map from a proto to a list of VirtualScopes
using ProtoMap = std::map<const DexProto*, VirtualScopes>;
// map from a name to a map of proto with that name
using SignatureMap = std::map<const DexString*, ProtoMap>;

//
// Entry points
//

/**
 * Given a scope it builds all the parent-children relationship known.
 * The walk stops once a DexClass is not found.
 * If all the code is known all classes will root to java.lang.Object.
 * If not some hierarcies will be "unknonw" (not completed)
 */
ClassHierarchy build_type_hierarchy(const Scope& scope);

void get_children(
    const ClassHierarchy& hierarchy,
    const DexType* type,
    std::vector<const DexType*>& children);

/**
 * Given a ClassHierarchy walk the java.lang.Object hierarchy building
 * all VirtualScope known.
 */
SignatureMap build_signature_map(const ClassHierarchy& class_hierarchy);

/**
 * Given a concrete DexMethod (must be a definition in a DexClass)
 * return the scope the method is in.
 */
const VirtualScope& find_virtual_scope(
    const SignatureMap& sig_map, const DexMethod* meth);

/*
 * Map from a class to the virtual scopes introduced by that class.
 * So every method at position 0 in the list of VirtualScope.methods
 * is a DexMethod in the vmethods of the class (DexType key).
 * VirtualScope.type and the DexType key are the same.
 * An entry for a type gives you back only the scopes rooted to
 * the type. So the number of VirtualScope is always smaller or
 * equals to the number of vmethods (unimplemented interface aside).
 */
using ClassScopes =
    std::map<const DexType*, std::vector<const VirtualScope*>>;

/*
 * Get the ClassScopes.
 */
ClassScopes get_class_scopes(
    const ClassHierarchy& hierarchy,
    const SignatureMap& sig_map);

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
  Timer timer("Devirtualizer inner");
  std::vector<DexMethod*> non_virtual;
  for (const auto& proto_it : sig_map) {
    for (const auto& scopes : proto_it.second) {
      for (const auto& scope : scopes.second) {
        if (type_class(scope.type) == nullptr ||
            is_interface(type_class(scope.type)) ||
            scope.interfaces.size() > 0) {
          continue;
        }
        for (const auto& meth : scope.methods) {
          if (!meth.first->is_concrete()) continue;
          if (meth.second == FINAL) {
            always_assert(scope.interfaces.size() == 0);
            always_assert(scope.methods.size() == 1);
            non_virtual.push_back(meth.first);
          }
        }
      }
    }
  }
  return non_virtual;
}

inline std::vector<DexMethod*> devirtualize(
    const std::vector<DexClass*>& scope) {
  Timer timer("Devirtualizer");
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
