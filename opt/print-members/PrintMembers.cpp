/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PrintMembers.h"

#include "DexUtil.h"
#include "Show.h"

void PrintMembersPass::run_pass(
  DexStoresVector& stores,
  ConfigFiles&,
  PassManager&
) {
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    for (auto const& cls : dex) {
      printf("class: %s\n", SHOW(cls));
      for (auto const& m : cls->get_sfields()) {
        printf("sfield: %s\n", SHOW(m));
      }
      for (auto const& m : cls->get_ifields()) {
        printf("ifield: %s\n", SHOW(m));
      }
      for (auto const& m : cls->get_dmethods()) {
        printf("dmethod: %s\n", SHOW(m));
      }
      for (auto const& m : cls->get_vmethods()) {
        printf("vmethod: %s\n", SHOW(m));
      }
    }
  }
}

static PrintMembersPass s_pass;
