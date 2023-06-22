/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassChecker.h"
#include "Walkers.h"

ClassChecker::ClassChecker() : m_good(true) {}

void ClassChecker::run(const Scope& scope) {
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (is_interface(cls) || is_abstract(cls)) {
      return;
    }
    for (auto* m : cls->get_dmethods()) {
      if (is_abstract(m)) {
        m_good = false;
        failed_classes.insert(cls);
        return;
      }
    }
    for (auto* m : cls->get_vmethods()) {
      if (is_abstract(m)) {
        m_good = false;
        failed_classes.insert(cls);
        return;
      }
    }
  });
}

std::ostringstream ClassChecker::print_failed_classes() {
  std::ostringstream oss;
  if (failed_classes.size() > 10) {
    oss << "Too many errors. Only printing out the first 10\n";
  }
  int counter = 0;
  oss << "Nonabstract classes with abstract methods: ";
  for (DexClass* fail : failed_classes) {
    oss << fail->get_deobfuscated_name_or_empty_copy() << "\n";
    counter++;
    if (counter == 10) {
      break;
    }
  }

  return oss;
}
