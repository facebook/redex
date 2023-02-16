/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "ClassHierarchy.h"
#include "DexClass.h"
#include "VirtualScope.h"

using TypeVector = std::vector<const DexType*>;
using InstanceOfTable = std::unordered_map<const DexType*, TypeVector>;
using TypeToTypeSet = std::unordered_map<const DexType*, TypeSet>;
using namespace virt_scope;

/**
 * TypeSystem
 * A class that computes information and caches on the current known state
 * of the universe given a Scope.
 * It provides common API to an object-oriented type system: inheritance
 * relationships, interface relationships, virtual scopes.
 *
 * NOTE: Computing virtual scopes is relatively expensive. If you only need
 * class-level and not method-level relationships, consider using ClassHierarchy
 * directly. Also, for method-level relationships, prefer the
 * MethodOverrideGraph over the VirtualScopes used here; the former is faster.
 */
class TypeSystem {
 private:
  static const TypeSet empty_set;
  static const TypeVector empty_vec;

  ClassScopes m_class_scopes;
  ClassHierarchy m_intf_children;
  InstanceOfTable m_instanceof_table;
  TypeToTypeSet m_interfaces;

 public:
  explicit TypeSystem(const Scope& scope);

  /**
   * Get the direct children of a given type.
   * The type must be a class (not an interface).
   */
  const TypeSet& get_children(const DexType* type) const {
    const auto& children = m_class_scopes.get_class_hierarchy().find(type);
    return children != m_class_scopes.get_class_hierarchy().end()
               ? children->second
               : empty_set;
  }

  /**
   * Get all the children of a given type.
   * The type must be a class (not an interface).
   */
  void get_all_children(const DexType* type, TypeSet& children) const {
    return ::get_all_children(
        m_class_scopes.get_class_hierarchy(), type, children);
  }

  /**
   * Return the chain of parents for a given type.
   * The type in question is included in the parent chain and it's
   * the last element in the returned vector.
   * The vector is ordered starting from the top type (java.lang.Object)
   * The type must be a class (not an interface).
   */
  const TypeVector& parent_chain(const DexType* type) const {
    const auto& parents = m_instanceof_table.find(type);
    if (parents == m_instanceof_table.end()) return empty_vec;
    return parents->second;
  }

  /**
   * Return all interfaces implemented by a given type.
   * A type must be a class (not an interface)
   */
  const TypeSet& get_implemented_interfaces(const DexType* type) const {
    const auto& intfs = m_interfaces.find(type);
    return intfs != m_interfaces.end() ? intfs->second : empty_set;
  }

  TypeSet get_implemented_interfaces(const TypeSet& types) const {
    TypeSet implemented_intfs;

    for (const auto& type : types) {
      const auto& cls_intfs = get_implemented_interfaces(type);
      implemented_intfs.insert(cls_intfs.begin(), cls_intfs.end());
    }

    return implemented_intfs;
  }

  /**
   * Returns only the interfaces that are implemented by the provided
   * classes.
   */
  TypeSet get_local_interfaces(const TypeSet& classes);

  /**
   * Return true if child is a subclass or equal to parent.
   * The type must be a class (not an interface).
   */
  bool is_subtype(const DexType* parent, const DexType* child) const {
    const auto& parent_it = m_instanceof_table.find(parent);
    const auto& child_it = m_instanceof_table.find(child);
    if (parent_it == m_instanceof_table.end() ||
        child_it == m_instanceof_table.end()) {
      return false;
    }
    const auto& p_chain = parent_it->second;
    const auto& c_chain = child_it->second;
    if (p_chain.size() > c_chain.size()) return false;
    return c_chain.at(p_chain.size() - 1) == parent;
  }

  /**
   * Return true if a given class implements a given interface.
   * The interface may be implemented via some parent of the class
   * or an interface DAG.
   */
  bool implements(const DexType* cls, const DexType* intf) const {
    const auto& implementors = m_class_scopes.get_interface_map().find(intf);
    if (implementors == m_class_scopes.get_interface_map().end()) return false;
    return implementors->second.count(cls) > 0;
  }

  /**
   * Return all classes that implement an interface.
   * The interface may be implemented via some parent of the class
   * or an interface DAG.
   * The implication is that all children of a type implementing an
   * interface will be included in the returning set.
   */
  const TypeSet& get_implementors(const DexType* intf) const {
    const auto& implementors = m_class_scopes.get_interface_map().find(intf);
    if (implementors == m_class_scopes.get_interface_map().end()) {
      return empty_set;
    }
    return implementors->second;
  }

  /**
   * Return the set of every parent interface of a given interface.
   * The type must be an interface (not a class).
   * The direct list of interfaces implemented can be retrived in the
   * DexClass.
   */
  void get_all_super_interfaces(const DexType* intf, TypeSet& supers) const;
  TypeSet get_all_super_interfaces(const DexType* intf) const;

  /**
   * Return the direct children of a given interface.
   * The type must be an interface (not a class).
   */
  const TypeSet& get_interface_children(const DexType* intf) const {
    const auto& children = m_intf_children.find(intf);
    if (children == m_intf_children.end()) return empty_set;
    return children->second;
  }

  /**
   * Return all the children of a given interface.
   * The type must be an interface (not a class).
   */
  void get_all_interface_children(const DexType* intf,
                                  TypeSet& children) const {
    const auto& direct_children = get_interface_children(intf);
    children.insert(direct_children.begin(), direct_children.end());
    for (const auto& child : direct_children) {
      get_all_interface_children(child, children);
    }
  }

  /**
   * Return the ClassScopes known when building the type system.
   * The ClassScopes lifetime is tied to that of the TypeSystem, as
   * such it should not exceed it.
   */
  const ClassScopes& get_class_scopes() const { return m_class_scopes; }

  /**
   * Given a DexMethod return the scope the method is in.
   */
  const VirtualScope* find_virtual_scope(const DexMethod* meth) const;
  InterfaceScope find_interface_scope(const DexMethod* meth) const {
    return m_class_scopes.find_interface_scope(meth);
  }

  /**
   * Given a set of types select the concrete methods invoked for those
   * types in a given scope.
   */
  void select_methods(const VirtualScope& scope,
                      const std::unordered_set<DexType*>& types,
                      std::unordered_set<DexMethod*>& methods) const;
  void select_methods(const InterfaceScope& scope,
                      const std::unordered_set<DexType*>& types,
                      std::unordered_set<DexMethod*>& methods) const;

  /**
   * Given a VirtualScope and a type return the list of methods that
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
  std::vector<const DexMethod*> select_from(const VirtualScope* scope,
                                            const DexType* type) const;

 private:
  void make_instanceof_interfaces_table();
  void make_interfaces_table(const DexType* type);
};
