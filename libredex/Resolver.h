/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"

#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <boost/functional/hash.hpp>

using MethodSet = std::unordered_set<DexMethod*>;

/**
 * Type of search to perform.
 * These flags direct the way lookup if performed up the hierarchy more than
 * the type of method to resolve.
 * Specifically, Direct and Static both look into the dmethods list, however
 * Static searches up the hierarchy whereas Direct only searches into the given
 * class.
 * In a sense they roughly match the opcode and the Dalvik resolution
 * semantic rather than the type of methods.
 */
enum class MethodSearch {
  Direct, // invoke-direct: private and init methods in class only
  Static, // invoke-static: dmethods in class and up the hierarchy
  Virtual, // invoke-virtual: vmethods in class and up the hierarchy
  Super, // invoke-super: vmethods up the hierarchy
  Any, // any method (vmethods or dmethods) in class and up the hierarchy
       // but not interfaces
  Interface, // invoke-interface: vmethods in interface class graph
  InterfaceVirtual // invoke-virtual but the final resolved method is interface
                   // method. Fallback to interface search when virtual search
                   // failed.
                   // This is added because we don't have Miranda methods in
                   // Redex but this case exist:
                   // Interface a { something(); }
                   // class b implements a {}
                   // class c extends b { something(){} }
                   // ... invoke-virtual b.something() ...
                   // MethodSearch::Virtual will return nullptr. So we added
                   // MethodSearch::InterfaceVirtual that can resolve to
                   // a.something()
};

struct MethodRefCacheKey {
  DexMethodRef* method;
  MethodSearch search;

  bool operator==(const MethodRefCacheKey& other) const {
    return method == other.method && search == other.search;
  }
};

struct MethodRefCacheKeyHash {
  std::size_t operator()(const MethodRefCacheKey& key) const {
    std::size_t seed = 0;
    boost::hash_combine(seed, key.method);
    boost::hash_combine(
        seed, static_cast<std::underlying_type_t<MethodSearch>>(key.search));
    return seed;
  }
};

using MethodRefCache =
    std::unordered_map<MethodRefCacheKey, DexMethod*, MethodRefCacheKeyHash>;

using ConcurrentMethodRefCache =
    ConcurrentMap<MethodRefCacheKey, DexMethod*, MethodRefCacheKeyHash>;

/**
 * Helper to map an opcode to a MethodSearch rule.
 */
inline MethodSearch opcode_to_search(const IROpcode opcode) {
  always_assert(opcode::is_an_invoke(opcode));
  switch (opcode) {
  case OPCODE_INVOKE_DIRECT:
    return MethodSearch::Direct;
  case OPCODE_INVOKE_STATIC:
    return MethodSearch::Static;
  case OPCODE_INVOKE_VIRTUAL:
    return MethodSearch::Virtual;
  case OPCODE_INVOKE_SUPER:
    return MethodSearch::Super;
  case OPCODE_INVOKE_INTERFACE:
    return MethodSearch::Interface;
  default:
    return MethodSearch::Any;
  }
}

/**
 * Helper to map an opcode to a MethodSearch rule.
 */
inline MethodSearch opcode_to_search(const IRInstruction* insn) {
  return opcode_to_search(insn->opcode());
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a method
 * definition in scope.
 * The lookup is performed according to the search rules specified via
 * MethodSearch.
 */
DexMethod* resolve_method(const DexClass*,
                          const DexString*,
                          const DexProto*,
                          MethodSearch search = MethodSearch::Any,
                          const DexMethod* caller = nullptr);

/**
 * Given a scope defined by DexClass, a name and a proto look for a vmethod
 * definition in scope.
 */
inline DexMethod* resolve_virtual(const DexClass* cls,
                                  const DexString* name,
                                  const DexProto* proto) {
  return resolve_method(cls, name, proto, MethodSearch::Virtual);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a vmethod
 * definition in scope.
 */
inline DexMethod* resolve_super(const DexClass* cls,
                                const DexString* name,
                                const DexProto* proto,
                                const DexMethod* caller) {
  return resolve_method(cls, name, proto, MethodSearch::Super, caller);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a dmethod
 * definition in class only.
 */
inline DexMethod* resolve_direct(const DexClass* cls,
                                 const DexString* name,
                                 const DexProto* proto) {
  return resolve_method(cls, name, proto, MethodSearch::Direct);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a dmethod
 * definition in scope.
 */
inline DexMethod* resolve_static(const DexClass* cls,
                                 const DexString* name,
                                 const DexProto* proto) {
  return resolve_method(cls, name, proto, MethodSearch::Static);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a vmethod
 * definition in the scope defined by the interface.
 */
inline DexMethod* resolve_interface_method(const DexClass* cls,
                                           const DexString* name,
                                           const DexProto* proto) {
  if (!is_interface(cls)) return nullptr;
  return resolve_method(cls, name, proto, MethodSearch::Interface);
}

/**
 * Resolve a method ref to its definition.
 * The search starts from the super for a non interface search and from
 * the super interfaces for interfaces.
 * If the type the method belongs to is unknown return nullptr.
 */
DexMethod* resolve_method_ref(const DexClass* cls,
                              const DexString* name,
                              const DexProto* proto,
                              MethodSearch search);

/**
 * Resolve a method to its definition. When searching for a definition of a
 * virtual callsite, we return one of the possible callees.
 *
 * - Handling searching of super class requires `caller` argument to be passed.
 *
 * - When this is used to search for any invoke other than invoke-super, if the
 * method is already a definition return itself.
 *
 * - If the type the method belongs to is unknown return nullptr.
 */
inline DexMethod* resolve_method(DexMethodRef* method,
                                 MethodSearch search,
                                 const DexMethod* caller = nullptr) {
  if (search == MethodSearch::Super) {
    if (caller) {
      // caller must be provided. This condition is here to be compatible with
      // old behavior.
      DexType* containing_type = caller->get_class();
      DexClass* containing_class = type_class(containing_type);
      if (containing_class == nullptr) return nullptr;
      DexType* super_class = containing_class->get_super_class();
      if (!super_class) return nullptr;
      // build method ref from parent class
      method = DexMethod::make_method(super_class, method->get_name(),
                                      method->get_proto());
    }
    // The rest is the same as virtual.
    search = MethodSearch::Virtual;
  }

  auto m = method->as_def();
  if (m) {
    return m;
  }
  auto cls = type_class(method->get_class());
  if (cls == nullptr) return nullptr;
  return resolve_method_ref(cls, method->get_name(), method->get_proto(),
                            search);
}

/**
 * Resolve a method and cache the mapping.
 * If the method is already a definition return itself.
 * If the type the method belongs to is unknown return nullptr.
 * This method takes a cache from refs to defs and populate it
 * to help speed up resolution.
 * When walking all the opcodes this method performs better avoiding lookup
 * of refs that had been resolved already.
 * Clients are responsible for the lifetime of the cache.
 *
 * Note that the cache is not thread-safe.
 */
inline DexMethod* resolve_method(DexMethodRef* method,
                                 MethodSearch search,
                                 MethodRefCache& ref_cache,
                                 const DexMethod* caller = nullptr) {
  if (search == MethodSearch::Super) {
    // We don't have cache for that since caller might be different.
    return resolve_method(method, search, caller);
  }
  auto m = method->as_def();
  if (m) {
    return m;
  }
  auto def = ref_cache.find(MethodRefCacheKey{method, search});
  if (def != ref_cache.end()) {
    return def->second;
  }
  auto mdef = resolve_method(method, search, caller);
  if (mdef != nullptr) {
    ref_cache.emplace(MethodRefCacheKey{method, search}, mdef);
  }
  return mdef;
}

/**
 * Resolve a method and cache the mapping. This method has the same behavior as
 * the other resolve_method with a cache, but this method is thread-safe.
 */
inline DexMethod* resolve_method(DexMethodRef* method,
                                 MethodSearch search,
                                 ConcurrentMethodRefCache& concurrent_ref_cache,
                                 const DexMethod* caller = nullptr) {
  if (search == MethodSearch::Super) {
    // We don't have cache for that since caller might be different.
    return resolve_method(method, search, caller);
  }
  auto m = method->as_def();
  if (m) {
    return m;
  }
  auto def =
      concurrent_ref_cache.get(MethodRefCacheKey{method, search}, nullptr);
  if (def != nullptr) {
    return def;
  }
  auto mdef = resolve_method(method, search, caller);
  if (mdef != nullptr) {
    concurrent_ref_cache.emplace(MethodRefCacheKey{method, search}, mdef);
  }
  return mdef;
}

/**
 * Given a scope defined by DexClass, a name and a proto look for the vmethod
 * on the top ancestor. Essentially finds where the method was introduced.
 * Stop the search when the type is unknown.
 * So effectively this returns the method on the top known ancestor.
 */
DexMethod* find_top_impl(const DexClass*, const DexString*, const DexProto*);

/**
 * Find where a method was introduced from an interface.
 * It may return a concrete method or a miranda depending on whether
 * the class where the interface is declared has a concrete method for the
 * interface method.
 */
DexMethod* find_top_intf_impl(const DexClass* cls,
                              const DexString* name,
                              const DexProto* proto);

/**
 * Type of fields to resolve.
 */
enum class FieldSearch { Static, Instance, Any };

/**
 * Given a scope, a field name and a field type search the class
 * hierarchy for a definition of the field
 */
DexField* resolve_field(const DexType*,
                        const DexString*,
                        const DexType*,
                        FieldSearch = FieldSearch::Any);

/**
 * Given a field, search its class hierarchy for the definition.
 * If the field is a definition already the field is returned otherwise a
 * lookup in the class hierarchy is performed looking for the definition.
 */
inline DexField* resolve_field(const DexFieldRef* field,
                               FieldSearch search = FieldSearch::Any) {
  if (field->is_def()) {
    return const_cast<DexField*>(static_cast<const DexField*>(field));
  }
  return resolve_field(field->get_class(), field->get_name(), field->get_type(),
                       search);
}
