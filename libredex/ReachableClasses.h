/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include "DexClass.h"
#include "DexUtil.h"

void init_reachable_classes(const Scope& scope, const JsonWrapper& config);

void recompute_reachable_from_xml_layouts(const Scope& scope,
                                          const std::string& apk_dir);

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
inline bool can_rename_if_also_renaming_xml(DexMember* member) {
  return member->rstate.can_rename_if_also_renaming_xml();
}

template <class DexMember>
inline bool is_serde(DexMember* member) {
  return member->rstate.is_serde();
}

template <class DexMember>
inline bool marked_by_string(DexMember* member) {
  return member->rstate.is_referenced_by_string();
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
