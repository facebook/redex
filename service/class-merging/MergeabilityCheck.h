/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

class RefChecker;

namespace class_merging {

using TypeSet = std::set<const DexType*, dextypes_comparator>;

struct ModelSpec;

class MergeabilityChecker {
 public:
  MergeabilityChecker(const Scope& scope,
                      const ModelSpec& spec,
                      const RefChecker& ref_checker,
                      const TypeSet& generated,
                      const TypeSet& to_merge)
      : m_scope(scope),
        m_spec(spec),
        m_ref_checker(ref_checker),
        m_generated(generated),
        m_to_merge(to_merge) {}
  /**
   * Try to identify types referenced by operations that Class Merging does not
   * support. Such operations include reflections, instanceof checks on
   * no-type-tag shapes.
   * Ideally, part of the checks we perform below should be enforced at Java
   * source level. That is we should restrict such use cases on the generated
   * Java classes. As a result, we can make those generated classes easier to
   * optimize by Class Merging.
   */
  TypeSet get_non_mergeables();

 private:
  const Scope& m_scope;
  const ModelSpec& m_spec;
  const RefChecker& m_ref_checker;
  const TypeSet& m_generated;
  const TypeSet& m_to_merge;

  void exclude_cannot_delete(TypeSet& non_mergeables);
  void exclude_unsupported_bytecode(TypeSet& non_mergeables);
  void exclude_static_fields(TypeSet& non_mergeables);
  void exclude_unsafe_sdk_and_store_refs(TypeSet& non_mergeables);
};

} // namespace class_merging
