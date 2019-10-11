/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "DexClass.h"
#include "DexUtil.h"

void init_reachable_classes(
    const Scope& scope,
    const JsonWrapper& config,
    const std::unordered_set<DexType*>& no_optimizations_anno);

void recompute_reachable_from_xml_layouts(const Scope& scope,
                                          const std::string& apk_dir);

// Note: The lack of convenience functions for DexType* is intentional. By doing
// so, it implies you need to nullptr check. Which is evil because it sprinkles
// nullptr checks everywhere.

template <class DexMember>
inline bool can_delete(DexMember* member) {
  return !member->is_external() && member->rstate.can_delete();
}

template <class DexMember>
inline bool root(DexMember* member) {
  return !can_delete(member);
}

template <class DexMember>
inline bool can_rename(DexMember* member) {
  return !member->is_external() && member->rstate.can_rename();
}

template <class DexMember>
inline bool can_delete_DEPRECATED(DexMember* member) {
  return member->rstate.can_delete_DEPRECATED();
}

template <class DexMember>
inline bool can_rename_DEPRECATED(DexMember* member) {
  return member->rstate.can_rename_DEPRECATED();
}

template <class DexMember>
inline bool is_serde(DexMember* member) {
  return member->rstate.is_serde();
}

// A temporary measure to allow the RenamerV2 pass to rename classes that would
// other not be renamable due to any top level blanket keep rules.
template <class DexMember>
inline bool can_rename_if_ignoring_blanket_keepnames(DexMember* member) {
  return can_rename_DEPRECATED(member) ||
         member->rstate.is_blanket_names_kept();
}

template <class DexMember>
inline bool has_keep(DexMember* member) {
  return member->rstate.has_keep();
}

template <class DexMember>
inline bool marked_by_string(DexMember* member) {
  return member->rstate.is_referenced_by_string();
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

/**
 * If this member is root or it has m_by_string be true.
 */
template <class DexMember>
inline bool root_or_string(DexMember* member) {
  return root(member) || marked_by_string(member);
}
