/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "DexClass.h"

class ClassChecker {
 public:
  ClassChecker();

  ClassChecker(const ClassChecker&) = delete;
  ClassChecker(ClassChecker&& other) = default;

  ClassChecker& operator=(const ClassChecker&) = delete;

  void run(const Scope& scope);

  bool fail() const { return !m_good; }

  std::ostringstream print_failed_classes();

 private:
  bool m_good;
  ConcurrentSet<DexClass*> failed_classes;
};
