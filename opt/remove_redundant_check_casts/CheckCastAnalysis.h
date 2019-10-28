/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "IRCode.h"

namespace check_casts {

namespace impl {

using CheckCastReplacements =
    std::vector<std::pair<MethodItemEntry*, boost::optional<IRInstruction*>>>;

class CheckCastAnalysis {

 public:
  explicit CheckCastAnalysis(DexMethod* method) : m_method(method){};
  const CheckCastReplacements collect_redundant_checks_replacement();

 private:
  DexMethod* m_method;
};

} // namespace impl

} // namespace check_casts
