/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "IRInstruction.h"

// Helper class to deal with (otherwise uninlinable) constructors that take a
// (newly created) object, and only use it to read ifields. For those
// constructors, we identify when we can replace the (newly created) object
// parameter with a sequence of field value parameters.
class ExpandableConstructorParams {
 private:
  // For each class, and each constructor, and each parameter, we record the
  // (ordered) list of ifields that are read from the parameter, if the
  // parameter doesn't otherwise escape, and the implied expanded constructor
  // arg list is not in conflict with any other constructor arg list.
  using ClassInfo = std::unordered_map<
      DexMethod*,
      std::unordered_map<param_index_t, std::vector<DexField*>>>;

  static std::vector<DexType*> get_expanded_args_vector(
      DexMethod* ctor,
      param_index_t param_index,
      const std::vector<DexField*>& fields);

  // Get or create the class-info for a given type.
  ClassInfo* get_class_info(DexType* type) const;

  // Given an earlier created expanded constructor method ref, fill in the code.
  DexMethod* make_expanded_ctor_concrete(DexMethodRef* expanded_ctor_ref);

 public:
  explicit ExpandableConstructorParams(const Scope& scope);

  // Try to create a method-ref that represents an expanded ctor, where a
  // particular parameter representing a (newly created) object gets replaced by
  // a sequence of field values used by the ctor.
  DexMethodRef* get_expanded_ctor_ref(DexMethod* ctor,
                                      param_index_t param_index,
                                      std::vector<DexField*>** fields) const;

  // Make sure that all newly used expanded ctors actually exist as concrete
  // methods.
  size_t flush(const Scope& scope);

 private:
  mutable ConcurrentMap<DexType*, std::shared_ptr<ClassInfo>> m_class_infos;
  // For each requested expanded constructor method ref, we remember the
  // original and ctor, and which parameter was expanded.
  using MethodParam = std::pair<DexMethod*, param_index_t>;
  mutable std::unordered_map<DexMethodRef*, MethodParam> m_candidates;
  mutable std::mutex m_candidates_mutex;
  // We keep track of deobfuscated ctor names already in use before the pass, to
  // avoid reusing them.
  std::unordered_set<const DexString*> m_deobfuscated_ctor_names;
};
