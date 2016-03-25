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
#include "ProguardLoader.h"

void init_reachable_classes(const Scope& scope, folly::dynamic& config, const std::vector<KeepRule>& proguard_rules);
void recompute_classes_reachable_from_code(const Scope& scope);

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

bool do_not_strip(DexField*);
bool do_not_strip(DexMethod*);
bool do_not_strip(DexClass*);
