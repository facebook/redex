/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <string>

#include "DexClass.h"
#include "DexUtil.h"

void init_reachable_classes(
    const Scope& scope,
    const Json::Value& config,
    const redex::ProguardConfiguration& pg_config,
    const std::unordered_set<DexType*>& no_optimizations_anno);
void recompute_classes_reachable_from_code(const Scope& scope);

// Note: The lack of convenience functions for DexType* is intentional. By doing
// so, it implies you need to nullptr check. Which is evil because it sprinkles
// nullptr checks everywhere.

// can_delete is to be deprecated function for determining if something can be
// deleted. We should find each and every use of can_delete and replace it with
// can_delete_if_unused with appropriate logic to ensure the class or member
// being deleted can be safely removed.
template <class DexMember>
inline bool can_delete(DexMember* member) {
  return member->rstate.can_delete();
}

template <class DexMember>
inline bool can_rename(DexMember* member) {
  return member->rstate.can_rename();
}

// A temporary measure to allow the RenamerV2 pass to rename classes that would
// other not be renamable due to any top level blanket keep rules.
template <class DexMember>
inline bool can_rename_if_ignoring_blanket_keepnames(DexMember* member) {
  return can_rename(member) || member->rstate.is_blanket_names_kept();
}

template <class DexMember>
inline bool keep(DexMember* member) {
  return member->rstate.keep();
}

template <class DexMember>
inline bool allowshrinking(DexMember* member) {
  return member->rstate.allowshrinking();
}

template <class DexMember>
inline bool allowobfuscation(DexMember* member) {
  return member->rstate.allowobfuscation();
}

template <class DexMember>
inline bool assumenosideeffects(DexMember* member) {
  return member->rstate.assumenosideeffects();
}

// Note: Redex currently doesn't implement allowoptimization, -keepnames,
// -keepclassmembernames, -keepclasseswithmembernames.

// root is an attempt to identify a root for reachability analysis by using any
// class or member that has keep set on it but does not have allowshrinking set
// on it.
template<class DexMember>
inline bool root(DexMember* member) {
  return keep(member) && !allowshrinking(member);
}
