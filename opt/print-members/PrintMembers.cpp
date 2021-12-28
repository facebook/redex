/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintMembers.h"

#include "DexUtil.h"
#include "Show.h"

void PrintMembersPass::run_pass(DexStoresVector& stores,
                                ConfigFiles&,
                                PassManager&) {
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    for (auto const& cls : dex) {
      if (m_config.only_these_classes.empty() ||
          m_config.only_these_classes.count(cls) > 0) {
        printf("class: %s\n", SHOW(cls));

        if (m_config.show_sfields) {
          for (auto const& m : cls->get_sfields()) {
            printf("sfield: %s\n", SHOW(m));
          }
        }

        if (m_config.show_ifields) {
          for (auto const& m : cls->get_ifields()) {
            printf("ifield: %s\n", SHOW(m));
          }
        }

        for (auto const& m : cls->get_dmethods()) {
          handle_method(m, "dmethod");
        }
        for (auto const& m : cls->get_vmethods()) {
          handle_method(m, "vmethod");
        }
      }
    }
  }
}

void PrintMembersPass::handle_method(DexMethod* m, const char* type) {
  if (m_config.only_these_methods.empty() ||
      m_config.only_these_methods.count(m) > 0) {
    printf("%s: %s\n", type, SHOW(m));
    const IRCode* code = m->get_code();
    if (m_config.show_code && code != nullptr) {
      printf("%s\n", SHOW(code));
    }
  }
}

static PrintMembersPass s_pass;
