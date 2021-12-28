/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include <set>
#include <unordered_map>

using TypeSet = std::set<const DexType*, dextypes_comparator>;

/**
 * DexType parent to children relationship
 * (child to parent is in DexClass)
 */
using ClassHierarchy = std::unordered_map<const DexType*, TypeSet>;

/**
 * Given a scope it builds all the parent-children relationship known.
 * The walk stops once a DexClass is not found.
 * If all the code is known all classes will root to java.lang.Object.
 * If not some hierarchies will be "unknown" (not completed)
 */
ClassHierarchy build_type_hierarchy(const Scope& scope);

/**
 * Return the direct children of a type.
 */
const TypeSet& get_children(const ClassHierarchy& hierarchy,
                            const DexType* type);

/**
 * Return all children down the hierarchy of a given type.
 */
void get_all_children(const ClassHierarchy& hierarchy,
                      const DexType* type,
                      TypeSet& children);

TypeSet get_all_children(const ClassHierarchy& hierarchy, const DexType* type);

/**
 * Map from each interface to the classes implementing that interface.
 * Interfaces are "flattened" so that a super interface maps to every
 * class implementing a deriving interface.
 */
using InterfaceMap = std::unordered_map<const DexType*, TypeSet>;

/**
 * Build an InterfaceMap given a class hierarchy.
 * If an interface does not have a DexClass and that interface implements
 * other interfaces that relationship is lost (unknown) so this builds a map
 * that is correct for all interfaces that have a DexClass and stops at that
 * interface otherwise.
 */
InterfaceMap build_interface_map(const ClassHierarchy& hierarchy);

/**
 * Return whether a given class implements a given interface.
 */
inline bool implements(const InterfaceMap& interfaces,
                       const DexType* cls,
                       const DexType* intf) {
  const auto& classes = interfaces.find(intf);
  return classes != interfaces.end() && classes->second.count(cls) > 0;
}

/**
 * Retrieve all the implementors of an interface and push them in the
 * provided set.
 */
void get_all_implementors(const Scope& scope,
                          const DexType* intf,
                          TypeSet& impls);

inline TypeSet get_all_implementors(const InterfaceMap& interfaces,
                                    const DexType* intf) {
  const auto& implementors = interfaces.find(intf);
  if (implementors == interfaces.end()) {
    return TypeSet();
  }
  return implementors->second;
}

/**
 * Helper to retrieve either the children of a concrete type or
 * all implementors of an interface.
 */
inline void get_all_children_or_implementors(
    const ClassHierarchy& ch,
    const Scope& scope,
    const DexClass* base_class,
    TypeSet& children_or_implementors) {
  if (is_interface(base_class)) {
    get_all_implementors(scope, base_class->get_type(),
                         children_or_implementors);
  } else {
    get_all_children(ch, base_class->get_type(), children_or_implementors);
  }
}

/**
 * Like find_collision, but don't report a match on `except`.
 */
DexMethod* find_collision_excepting(const ClassHierarchy& ch,
                                    const DexMethod* except,
                                    const DexString* name,
                                    const DexProto* proto,
                                    const DexClass* cls,
                                    bool is_virtual,
                                    bool check_direct);

/**
 * Given a name and a proto find a possible collision with methods with
 * the same name and proto.
 * The search is performed in the vmethods or dmethods space according to
 * the is_virtual argument.
 * When searching in the virtual methods space the search is performed up and
 * down the hierarchy chain. When in the direct method space only the current
 * class is searched.
 */
inline DexMethod* find_collision(const ClassHierarchy& ch,
                                 const DexString* name,
                                 const DexProto* proto,
                                 const DexClass* cls,
                                 bool is_virtual) {
  return find_collision_excepting(ch, nullptr, name, proto, cls, is_virtual,
                                  false);
}
