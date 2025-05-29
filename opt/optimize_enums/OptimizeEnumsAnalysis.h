/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DeterministicContainers.h"
#include "DexClass.h"

namespace optimize_enums {

namespace impl {

// Forward declaration.
class Analyzer;

} // namespace impl

class OptimizeEnumsAnalysis final {
 public:
  ~OptimizeEnumsAnalysis();

  explicit OptimizeEnumsAnalysis(
      const DexClass* enum_cls,
      const UnorderedMap<const DexMethod*, uint32_t>& ctor_to_arg_ordinal);

  void collect_ordinals(UnorderedMap<DexField*, size_t>& enum_field_to_ordinal);

 private:
  std::unique_ptr<impl::Analyzer> m_analyzer;
  const DexClass* m_cls;
};

} // namespace optimize_enums
