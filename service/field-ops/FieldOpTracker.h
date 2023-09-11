/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "DexClass.h"

#include <unordered_map>

namespace field_op_tracker {

struct FieldStats {
  // Number of instructions which read a field in the entire program.
  size_t reads{0};
  // Number of instructions which write a field in the entire program.
  size_t writes{0};
  // Number of instructions which write this field inside of a <clinit> or
  // <init> of the same declaring type, where the field is static for <clinit>
  // and an instance field accessed via the receiver parameter for <init>.
  // init_writes are also included in writes.
  size_t init_writes{0};

  FieldStats& operator+=(const FieldStats& that) {
    reads += that.reads;
    writes += that.writes;
    init_writes += that.init_writes;
    return *this;
  }
};

// Certain types don't have lifetimes, or at least nobody should depend on them.
class TypeLifetimes {
 private:
  std::unordered_set<const DexType*> m_ignored_types;
  const DexType* m_java_lang_Enum;
  mutable ConcurrentMap<const DexMethodRef*, boost::optional<bool>> m_cache;

 public:
  TypeLifetimes();
  bool has_lifetime(const DexType* t) const;
};

using FieldStatsMap = std::unordered_map<DexField*, FieldStats>;

FieldStatsMap analyze(const Scope& scope);

struct FieldWrites {
  // All fields to which some potentially non-zero value is written.
  ConcurrentSet<DexField*> non_zero_written_fields;
  // All fields to which some non-vestigial object is written.
  // We say an object is "vestigial" when the only escaping reference to it is
  // stored in a particular field. In other words, the only way to retrieve and
  // observe such an object is by reading from that field. Then, if that field
  // is unread, we can remove the iput/sput to it, as it is not possible that
  // the object's lifetime can be observed by a weak reference, at least after
  // the storing method returns.
  ConcurrentSet<DexField*> non_vestigial_objects_written_fields;
};

void analyze_writes(const Scope& scope,
                    const FieldStatsMap& field_stats,
                    const TypeLifetimes* type_lifetimes,
                    FieldWrites* res);
} // namespace field_op_tracker
