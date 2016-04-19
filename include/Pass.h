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
#include <vector>
#include <folly/dynamic.h>

#include "ConfigFiles.h"

class DexClasses;
class DexClass;
using DexClassesVector = std::vector<DexClasses>;
using Scope = std::vector<DexClass*>;

class Pass {
 public:
  enum DoesNotSync {};

  Pass(std::string name)
     : m_config(nullptr),
       m_name(name),
       m_assumes_sync(true) {}

  Pass(std::string name, DoesNotSync)
     : m_config(nullptr),
       m_name(name),
       m_assumes_sync(false) {}

  virtual ~Pass() {}

  bool assumes_sync() const { return m_assumes_sync; }
  std::string name() const { return m_name; }

  virtual void run_pass(DexClassesVector&, ConfigFiles&) = 0;

  // configuration data
  folly::dynamic m_config;

 private:
  std::string m_name;
  const bool m_assumes_sync;
};
