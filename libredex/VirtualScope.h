/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "ClassHierarchy.h"
#include "Timer.h"
#include <vector>
#include <map>
#include <unordered_map>


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
  // implements an unknown interface in which case the entire branch
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
 * and possibly any interface that exposes bool equals(Object).
 *
 * IMPORTANT: A top method in the list of methods for a VirtualScope may not
 * be a definition when the method is a pure miranda, that is, when the method
 * is a missing implementation of an interface for that VirtualScope. e.g.:
 * interface I { void m(); }
 * abstract class A implements I {}
 * class B extends A { void m(); }
 * a VirtualScope exists for A.m() in which a DexMethod (A.m()) is at
 * the top of the VirtualScope method list and that DexMethod is not
 * a definition.
 */
struct VirtualScope {
  // root type for the VirtualScope
  const DexType* type;
  // list of methods in scope methods[0].first->get_class() == type
  std::vector<VirtualMethod> methods;
  // interface set the VirtualScope contributes to
  TypeSet interfaces;
};

using InterfaceScope = std::vector<const VirtualScope*>;

/**
 * Return true if a VirtualScope can be renamed.
 */
bool can_rename_scope(const VirtualScope* scope);

/**
 * Return true if a VirtualScope contributes to interface resolution.
 */
inline bool is_impl_scope(const VirtualScope* scope) {
  return scope->interfaces.size() > 0;
}

/**
 * Return true if a VirtualScope is composed by a single non impl method.
 * Effectively if the method is devirtualizable.
 */
inline bool is_non_virtual_scope(const VirtualScope* scope) {
  if (scope->methods[0].second ==
      (VirtualFlags::FINAL | VirtualFlags::TOP_DEF)) {
    always_assert(scope->methods.size() == 1);
    always_assert(!is_impl_scope(scope));
    return true;
  }
  return false;
}

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
using ProtoMap = std::map<const DexProto*, VirtualScopes, dexprotos_comparator>;
// map from a name to a map of proto with that name
using SignatureMap =
    std::map<const DexString*, ProtoMap, dexstrings_comparator>;

//
// Entry points
//

/**
 * Given a ClassHierarchy walk the java.lang.Object hierarchy building
 * all VirtualScope known.
 */
SignatureMap build_signature_map(const ClassHierarchy& class_hierarchy);

/**
 * Given a DexMethod return the scope the method is in.
 */
const VirtualScope& find_virtual_scope(
    const SignatureMap& sig_map, const DexMethod* meth);

/**
 * Given a VirtualScope and a type, return the list of methods that
 * could bind for that type in that scope.
 * There is no specific order to the methods returned.
 * Consider
 * class A { void m() {} }
 * class B extends A { void m() {} }
 * class C extends B { void m() {} }
 * class D extends C { void m() {} }
 * class E extends A { void m() {} }
 * The Virtual scope for m() starts in A.m() and contains all the m()
 * in the A hierarchy.
 * A call to select_from() with C with return only C.m() and D.m() which
 * are the only 2 methods in scope for C.
 */
std::vector<const DexMethod*> select_from(
    const VirtualScope* scope, const DexType* type);

/*
 * Map from a class to the virtual scopes introduced by that class.
 * So every method at position 0 in the list of VirtualScope.methods
 * is a DexMethod in the vmethods of the class (DexType key).
 * VirtualScope.type and the DexType key are the same.
 * An entry for a type gives you back only the scopes rooted to
 * the type. So the number of VirtualScope is always smaller or
 * equals to the number of vmethods (unimplemented interface aside).
 */
using Scopes = std::unordered_map<
    const DexType*, std::vector<const VirtualScope*>>;
using InterfaceScopes = std::unordered_map<
    const DexType*, std::vector<std::vector<const VirtualScope*>>>;

class ClassScopes {
 private:
  static const std::vector<const VirtualScope*> empty_scope;
  static const std::vector<std::vector<const VirtualScope*>>
      empty_interface_scope;

  Scopes m_scopes;
  InterfaceScopes m_interface_scopes;
  ClassHierarchy m_hierarchy;
  InterfaceMap m_interface_map;
  SignatureMap m_sig_map;

 public:
  explicit ClassScopes(const Scope& scope);

  /**
   * Return the vector of VirtualScope for the given type.
   * The vector lifetime is tied to that of the ClassScope as such it should
   * not exceed it.
   */
  const std::vector<const VirtualScope*>& get(const DexType* type) const {
    const auto& scopes_it = m_scopes.find(type);
    if (scopes_it == m_scopes.end()) {
      return empty_scope;
    }
    return scopes_it->second;
  }

  /**
   * Return all the interface scopes across the class hierarchy.
   * Each vector is effectively the scope of each branch where the
   * interface is implemented.
   * The vector lifetime is tied to that of the ClassScope as such it should
   * not exceed it.
   */
  const std::vector<InterfaceScope>& get_interface_scopes(
      const DexType* type) const {
    const auto& scopes_it = m_interface_scopes.find(type);
    if (scopes_it == m_interface_scopes.end()) {
      return empty_interface_scope;
    }
    return scopes_it->second;
  }

  /**
   * Walk all interface scopes calling the walker with a list of
   * scopes and an interface set for each pair of (method_name, method_sig).
   */
  template <class AllInterfaceScopesWalkerFn =
      void(const DexString*,
          const DexProto*,
          const std::vector<const VirtualScope*>&,
          const TypeSet&)>
  void walk_all_intf_scopes(AllInterfaceScopesWalkerFn walker) const {
    for (const auto& names_it : m_sig_map) {
      for (const auto sig_it : names_it.second) {
        std::vector<const VirtualScope*> intf_scopes;
        TypeSet intfs;
        for (auto& scope : sig_it.second) {
          redex_assert(type_class(scope.type) != nullptr);
          if (scope.interfaces.empty()) continue;
          intf_scopes.emplace_back(&scope);
          intfs.insert(scope.interfaces.begin(), scope.interfaces.end());
        }
        if (intf_scopes.empty()) continue;
        walker(names_it.first, sig_it.first, intf_scopes, intfs);
      }
    }
  }

  /**
   * Walk all VirtualScope and call the walker function for each scope.
   * The walk is top down the class hierarchy starting from the
   * specified type.
   */
  template <class VirtualScopeWalkerFn =
      void(const DexType*, const VirtualScope*)>
  void walk_virtual_scopes(
      const DexType* type,
      VirtualScopeWalkerFn walker) const {
    const auto& scopes_it = m_scopes.find(type);
    // first walk all scopes in type
    if (scopes_it != m_scopes.end()) {
      for (const auto& scope : scopes_it->second) {
        walker(type, scope);
      }
    }
    always_assert_log(
        m_hierarchy.find(type) != m_hierarchy.end(),
        "no entry in ClassHierarchy for type %s\n", SHOW(type));
    // recursively call for each child
    for (const auto& child : m_hierarchy.at(type)) {
      walk_virtual_scopes(child, walker);
    }
  }

  /**
   * Walk every VirtualScope starting from java.lang.Object and call the walker
   * function for each scope.
   */
  template <class VirtualScopeWalkerFn =
      void(const DexType*, const VirtualScope*)>
  void walk_virtual_scopes(VirtualScopeWalkerFn walker) const {
    walk_virtual_scopes(get_object_type(), walker);
  }

  /**
   * Walk every class scope calling the walker function for each class.
   * The walk is top down the class hierarchy starting from the given type.
   */
  template <class VirtualScopesWalkerFn =
      void(const DexType*, const std::vector<const VirtualScope*>&)>
  void walk_class_scopes(
      const DexType* type,
      VirtualScopesWalkerFn walker) const {
    const auto& scopes_it = m_scopes.find(type);
    // first walk all scopes in type
    if (scopes_it != m_scopes.end()) {
      walker(type, scopes_it->second);
    }
    always_assert_log(
        m_hierarchy.find(type) != m_hierarchy.end(),
        "no entry in ClassHierarchy for type %s\n", SHOW(type));
    // recursively call for each child
    for (const auto& child : m_hierarchy.at(type)) {
      walk_class_scopes(child, walker);
    }
  }

  /**
   * Walk every class scope calling the walker function for each class.
   * The walk is top down the class hierarchy starting from java.lang.Object.
   */
  template <class VirtualScopesWalkerFn =
      void(const DexType*, const std::vector<const VirtualScope*>&)>
  void walk_class_scopes(VirtualScopesWalkerFn walker) const {
    walk_class_scopes(get_object_type(), walker);
  }

  /**
   * Given a DexMethod return the scope the method is in.
   */
  const VirtualScope& find_virtual_scope(const DexMethod* meth) const {
    return ::find_virtual_scope(m_sig_map, meth);
  }

  /**
   * Given a DexMethod return the scope the method is in.
   */
  InterfaceScope find_interface_scope(const DexMethod* meth) const;

  /**
   * Return the ClassHierarchy known when building the scopes.
   * The ClassHierarchy lifetime is tied to that of the ClassScopes, as
   * such it should not exceed it.
   */
  const ClassHierarchy& get_class_hierarchy() const {
    return m_hierarchy;
  }

  /**
   * Return the InterfaceMap known when building the scopes.
   * The InterfaceMap lifetime is tied to that of the ClassScopes, as
   * such it should not exceed it.
   */
  const InterfaceMap& get_interface_map() const {
    return m_interface_map;
  }

  /**
   * Return the SignatureMap known when building the scopes.
   * The SignatureMap lifetime is tied to that of the ClassScopes, as
   * such it should not exceed it.
   */
  const SignatureMap& get_signature_map() const {
    return m_sig_map;
  }

 private:
  void build_class_scopes(const DexType* type);
  void build_interface_scopes();
};

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
          if (meth.second != FINAL) {
            break;
          }
          always_assert(scope.interfaces.size() == 0);
          non_virtual.push_back(meth.first);
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

inline std::unordered_set<const DexMethod*> find_non_overridden_virtuals(
    const SignatureMap& sig_map) {
  std::unordered_set<const DexMethod*> non_overridden_virtuals;
  for (const auto& proto_it : sig_map) {
    for (const auto& scopes : proto_it.second) {
      for (const auto& scope : scopes.second) {
        if (type_class(scope.type) == nullptr ||
            is_interface(type_class(scope.type))) {
          continue;
        }
        for (const auto& meth : scope.methods) {
          if (meth.second & FINAL) {
            non_overridden_virtuals.emplace(meth.first);
          }
        }
      }
    }
  }
  return non_overridden_virtuals;
}

inline std::unordered_set<const DexMethod*> find_non_overridden_virtuals(
    const std::vector<DexClass*>& scope) {
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  auto signature_map = build_signature_map(class_hierarchy);
  return find_non_overridden_virtuals(signature_map);
}

inline bool can_devirtualize(SignatureMap& sig_map, DexMethod* meth) {
  always_assert(meth->is_virtual());
  auto& proto_map = sig_map[meth->get_name()];
  auto& scopes = proto_map[meth->get_proto()];
  for (const auto& scope : scopes) {
    if (scope.type != meth->get_class()) {
      continue;
    }

    for (const auto& m : scope.methods) {
      if (!m.first->is_concrete()) continue;
      if (m.second != FINAL) {
        break;
      }
      always_assert(scope.interfaces.size() == 0);
      return true;
    }
  }
  return false;
}

/**
 * Return the list of virtual methods for a given type.
 * If the type is java.lang.Object and it is not known (no DexClass for it)
 * it generates fictional methods for it.
 */
const std::vector<DexMethod*>& get_vmethods(const DexType* type);
