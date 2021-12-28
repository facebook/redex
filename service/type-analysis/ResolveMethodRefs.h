/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "GlobalTypeAnalyzer.h"
#include "IRInstruction.h"
#include "PassManager.h"

class ResolveMethodRefs final {
 public:
  ResolveMethodRefs(const Scope& scope,
                    const type_analyzer::global::GlobalTypeAnalyzer& gta);

  void report(PassManager& mgr) const;

 private:
  void analyze_method(DexMethod* method,
                      const type_analyzer::local::LocalTypeAnalyzer& lta);
  size_t m_num_resolved_kt_non_capturing_lambda_calls = 0;
};
