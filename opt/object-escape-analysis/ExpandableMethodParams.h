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

// Helper class to deal with methods that take a
// (newly created) object, and only use it to read ifields. For those
// methods, we identify when we can replace the (newly created) object
// parameter with a sequence of field value parameters.
class ExpandableMethodParams {
  // For each (declaring-type, rtype, method-name) tuple, and each parameter, we
  // record the (ordered) list of ifields that are read from the parameter, if
  // the parameter doesn't otherwise escape, and if the implied expanded arg
  // list is not in conflict with any other arg list.
 public:
  struct MethodKey {
    DexType* type;
    DexType* rtype;
    const DexString* name;
    bool operator==(const MethodKey& other) const {
      return type == other.type && rtype == other.rtype && name == other.name;
    }
    static MethodKey from_method(DexMethod* method) {
      return (MethodKey){method->get_class(), method->get_proto()->get_rtype(),
                         method->get_name()};
    }
  };

  struct MethodKeyHash {
    size_t operator()(const MethodKey& key) const {
      size_t hash = 0;
      boost::hash_combine(hash, key.type);
      boost::hash_combine(hash, key.rtype);
      boost::hash_combine(hash, key.name);
      return hash;
    }
  };

 private:
  using MethodInfo = std::unordered_map<
      DexMethod*,
      std::unordered_map<param_index_t, std::vector<DexField*>>>;

  static std::vector<DexType*> get_expanded_args_vector(
      DexMethod* method,
      param_index_t param_index,
      const std::vector<DexField*>& fields);

  // create the method-info for a given type, method-name, rtype.
  MethodInfo create_method_info(const MethodKey& key) const;

  // Get or create the method-info for a given type, method-name, rtype.
  const MethodInfo* get_method_info(const MethodKey& key) const;

  // Given an earlier created expanded method ref, fill in the code.
  DexMethod* make_expanded_method_concrete(DexMethodRef* expanded_method_ref);

 public:
  explicit ExpandableMethodParams(const Scope& scope);

  // Try to create a method-ref that represents an expanded method, where a
  // particular parameter representing a (newly created) object gets replaced by
  // a sequence of field values used by the ctor.
  DexMethodRef* get_expanded_method_ref(
      DexMethod* method,
      param_index_t param_index,
      std::vector<DexField*> const** fields = nullptr) const;

  // Make sure that all newly used expanded ctors actually exist as concrete
  // methods.
  size_t flush(const Scope& scope);

 private:
  mutable InsertOnlyConcurrentMap<MethodKey, MethodInfo, MethodKeyHash>
      m_method_infos;
  // For each requested expanded method ref, we remember the
  // original method, and which parameter was expanded.
  using MethodParam = std::pair<DexMethod*, param_index_t>;
  mutable std::unordered_map<DexMethodRef*, MethodParam> m_candidates;
  mutable std::mutex m_candidates_mutex;
  // We keep track of deobfuscated method names already in use before the
  // pass, to avoid reusing them.
  std::unordered_set<const DexString*> m_deobfuscated_method_names;
};
