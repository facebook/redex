/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "Debug.h"
#include "DexClass.h"

class PassManager;
class IRInstruction;

class KotlinInstanceRewriter {
 public:
  struct Stats {
    size_t kotlin_new_instance{0};
    size_t kotlin_new_instance_which_escapes{0};
    size_t kotlin_instances_with_single_use{0};
    size_t kotlin_instance_fields_removed{0};
    size_t kotlin_new_inserted{0};
    size_t excludable_kotlin_lambda{0};
    size_t kotlin_lambda_without_instance{0};
    Stats& operator+=(const Stats& that) {
      kotlin_new_instance += that.kotlin_new_instance;
      kotlin_new_instance_which_escapes +=
          that.kotlin_new_instance_which_escapes;
      kotlin_instances_with_single_use += that.kotlin_instances_with_single_use;
      kotlin_instance_fields_removed += that.kotlin_instance_fields_removed;
      kotlin_new_inserted += that.kotlin_new_inserted;
      excludable_kotlin_lambda += that.excludable_kotlin_lambda;
      kotlin_lambda_without_instance += that.kotlin_lambda_without_instance;
      return *this;
    }
    // Updates metrics tracked by \p mgr corresponding to these statistics.
    // Simultaneously prints the statistics via TRACE.
    void report(PassManager& mgr) const;
  };

  KotlinInstanceRewriter() {
    m_instance = DexString::make_string("INSTANCE");
    always_assert(m_instance);
  }

  // Collect Kotlin noncapturing Lambda which would have an INSTANCE field of
  // the same type and is initilized in <clinit>. Collect all such Lambda. Map
  // contains the field (that contains INSTANCE) and {insn, method} where it is
  // read (or used).
  // - is_excludable: returns true if the class is excludable (e.g., hot lambda)
  // - exclude_excludable: if true, excludable classes are skipped
  Stats collect_instance_usage(
      const Scope& scope,
      ConcurrentMap<DexFieldRef*,
                    std::set<std::pair<IRInstruction*, DexMethod*>>>&
          concurrent_instance_map,
      std::function<bool(DexClass*)> is_excludable,
      bool exclude_excludable);

  // Filter out any INSTANCE that might escape and whose use we might not be
  // able to track
  Stats remove_escaping_instance(
      const Scope& scope,
      ConcurrentMap<DexFieldRef*,
                    std::set<std::pair<IRInstruction*, DexMethod*>>>&
          concurrent_instance_map);

  // Remove INSTANCE re-uses and re-write it with a new object. This would allow
  // subsequnt passes to optimize it.
  Stats transform(
      ConcurrentMap<DexFieldRef*,
                    std::set<std::pair<IRInstruction*, DexMethod*>>>&
          concurrent_instance_map);

 private:
  const size_t max_no_of_instance = 1;
  const DexString* m_instance = nullptr;
};
