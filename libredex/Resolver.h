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
#include "DexUtil.h"
#include "IRInstruction.h"

#include <unordered_map>
#include <unordered_set>


using MethodRefCache = std::unordered_map<DexMethodRef*, DexMethod*>;
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
  Any, // any method (vmethods or dmethods) in class and up the hierarchy
       // but not interfaces
  Interface // invoke-interface: vmethods in interface class graph
};

/**
 * Helper to map an opcode to a MethodSearch rule.
 */
inline MethodSearch opcode_to_search(const IRInstruction* insn) {
  auto opcode = insn->opcode();
  always_assert(is_invoke(opcode));
  switch (opcode) {
  case OPCODE_INVOKE_DIRECT:
    return MethodSearch::Direct;
  case OPCODE_INVOKE_STATIC:
    return MethodSearch::Static;
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
    return MethodSearch::Virtual;
  case OPCODE_INVOKE_INTERFACE:
    return MethodSearch::Interface;
  default:
    return MethodSearch::Any;
  }
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a method
 * definition in scope.
 * The lookup is performed according to the search rules specified via
 * MethodSearch.
 */
DexMethod* resolve_method(
    const DexClass*, const DexString*,
    const DexProto*, MethodSearch search = MethodSearch::Any);

/**
 * Given a scope defined by DexClass, a name and a proto look for a vmethod
 * definition in scope.
 */
inline DexMethod* resolve_virtual(
    const DexClass* cls, const DexString* name, const DexProto* proto) {
  return resolve_method(cls, name, proto, MethodSearch::Virtual);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a dmethod
 * definition in class only.
 */
inline DexMethod* resolve_direct(
    const DexClass* cls, const DexString* name, const DexProto* proto) {
  return resolve_method(cls, name, proto, MethodSearch::Direct);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a dmethod
 * definition in scope.
 */
inline DexMethod* resolve_static(
    const DexClass* cls, const DexString* name, const DexProto* proto) {
  return resolve_method(cls, name, proto, MethodSearch::Static);
}

/**
 * Given a scope defined by DexClass, a name and a proto look for a vmethod
 * definition in the scope defined by the interface.
 */
inline DexMethod* resolve_interface_method(
    const DexClass* cls, const DexString* name, const DexProto* proto) {
  if (!is_interface(cls)) return nullptr;
  return resolve_method(cls, name, proto, MethodSearch::Interface);
}

/**
 * Resolve a method ref to its definition.
 * The search starts from the super for a non interface search and from
 * the super interfaces for interfaces.
 * If the type the method belongs to is unknown return nullptr.
 */
DexMethod* resolve_method_ref(
    const DexClass* cls,
    const DexString* name,
    const DexProto* proto,
    MethodSearch search);

/**
 * Resolve a method to its definition.
 * If the method is already a definition return itself.
 * If the type the method belongs to is unknown return nullptr.
 */
inline DexMethod* resolve_method(DexMethodRef* method, MethodSearch search) {
  if (method->is_def()) return static_cast<DexMethod*>(method);
  auto cls = type_class(method->get_class());
  if (cls == nullptr) return nullptr;
  return resolve_method_ref(cls, method->get_name(), method->get_proto(), search);
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
 */
inline DexMethod* resolve_method(
    DexMethodRef* method, MethodSearch search, MethodRefCache& ref_cache) {
  if (method->is_def()) return static_cast<DexMethod*>(method);
  auto def = ref_cache.find(method);
  if (def != ref_cache.end()) {
    return def->second;
  }
  auto mdef = resolve_method(method, search);
  if (mdef != nullptr) {
    ref_cache[method] = mdef;
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
DexMethod* find_top_intf_impl(
    const DexClass* cls, const DexString* name, const DexProto* proto);

/**
 * Type of fields to resolve.
 */
enum class FieldSearch {
  Static, Instance, Any
};

/**
 * Given a scope, a field name and a field type search the class
 * hierarchy for a definition of the field
 */
DexField* resolve_field(
  const DexType*,
  const DexString*,
  const DexType*,
  FieldSearch = FieldSearch::Any);

/**
 * Given a field, search its class hierarchy for the definition.
 * If the field is a definition already the field is returned otherwise a
 * lookup in the class hierarchy is performed looking for the definition.
 */
inline DexField* resolve_field(
    DexFieldRef* field, FieldSearch search = FieldSearch::Any) {
  if (field->is_def()) {
    return static_cast<DexField*>(field);
  }
  return resolve_field(
      field->get_class(), field->get_name(), field->get_type(), search);
}
