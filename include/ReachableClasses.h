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
#include "ProguardLoader.h"

void init_reachable_classes(const Scope& scope, folly::dynamic& config,
    const std::vector<KeepRule>& proguard_rules,
    const std::unordered_set<DexType*>& no_optimizations_anno);
void recompute_classes_reachable_from_code(const Scope& scope);
void reportReachableClasses(const Scope& scope, std::string reportFileName);
void init_seed_classes(const std::string seeds_filename);

/* Note-
 * The lack of convenience functions for DexType* is
 * intentional.  By doing so, it implies you need to
 * nullptr check.  Which is evil because it sprinkles
 * nullptr checks everywhere.
 */
template<class DexMember>
inline bool can_delete(DexMember* member) { return member->rstate.can_delete(); }

template<class DexMember>
inline bool can_rename(DexMember* member) { return member->rstate.can_rename(); }

template<class DexMember>
inline bool is_seed(DexMember* member) { return member->rstate.is_seed(); }

// Check to see if a class can be removed. At a later stage when we
// are sure is_seed has 100% coverage of kept classes we can drop the
// can_delete check.
inline bool can_remove_class(DexClass* clazz) {
  if (is_seed(clazz) && can_delete(clazz)) {
      TRACE(PGR, 1, "Catch by seed class: %s\n",
          clazz->get_type()->get_name()->c_str());
  }
  if (!is_seed(clazz) && !can_delete(clazz)) {
    std::string name = clazz->get_type()->get_name()->c_str();
    if (name.find("$") == std::string::npos)
      TRACE(PGR, 1, "Catch by RF: %s\n",
          clazz->get_type()->get_name()->c_str());
  }
  return can_delete(clazz) && !is_seed(clazz);
}

bool do_not_strip(DexField*);
bool do_not_strip(DexMethod*);
bool do_not_strip(DexClass*);
