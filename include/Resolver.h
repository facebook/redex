// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "DexClass.h"
#include "DexUtil.h"

#include <unordered_map>
#include <unordered_set>


using MethodRefCache = std::unordered_map<DexMethod*, DexMethod*>;
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
  Any // any method (vmethods or dmethods) in class and up the hierarchy
};

/**
 * Helper to map an opcode to a MethodSearch rule.
 */
inline MethodSearch opcode_to_search(DexInstruction* insn) {
  auto opcode = insn->opcode();
  always_assert(is_invoke(opcode));
  switch (opcode) {
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_DIRECT_RANGE:
    return MethodSearch::Direct;
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_STATIC_RANGE:
    return MethodSearch::Static;
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_VIRTUAL_RANGE:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_SUPER_RANGE:
    return MethodSearch::Virtual;
  default:
    // TODO: sort out the interface story.
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
 * Resolve a method to its definition.
 * If the method is already a definition return itself.
 * If the type the method belongs to is unknown return nullptr.
 */
inline DexMethod* resolve_method(DexMethod* method, MethodSearch search) {
  if (method->is_def()) return method;
  auto cls = type_class(method->get_class());
  if (cls == nullptr) return nullptr;
  return resolve_method(cls, method->get_name(), method->get_proto(), search);
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
    DexMethod* method, MethodSearch search, MethodRefCache& ref_cache) {
  if (method->is_def()) return method;
  auto cls = type_class(method->get_class());
  if (cls == nullptr) return nullptr;
  auto def = ref_cache.find(method);
  if (def != ref_cache.end()) {
    return def->second;
  }
  auto mdef = resolve_method(
      cls, method->get_name(), method->get_proto(), search);
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
 * Resolve an interface method ref to the real method if one exists. Return the
 * new method if one is found, the original method if the binding was
 * correct or nullptr if the method is unknown.
 * An unknown binding is to a method outside the set of methods defined in
 * the app (say, to a java library).
 * interface A { void m() {} }
 * interface B extends A {}
 * some code may have a method ref like
 * B.m()
 * though m() is not defined on B. This function will return A.m()
 */
DexMethod* resolve_intf_methodref(DexMethod* meth);

/**
 * Like find_collision, but don't report a match on `except`.
 */
DexMethod* find_collision_excepting(const DexMethod* except,
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
inline DexMethod* find_collision(
    const DexString* name, const DexProto* proto,
    const DexClass* cls, bool is_virtual) {
  return find_collision_excepting(nullptr, name, proto, cls, is_virtual, false);
}

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
    DexField* field, FieldSearch = FieldSearch::Any) {
  if (field->is_def()) return field;
  return resolve_field(
      field->get_class(), field->get_name(), field->get_type());
}

