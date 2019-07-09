/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "EnumUpcastAnalysis.h"
#include "Resolver.h"

namespace optimize_enums {

/**
 * We want to remove the generated static methods `SubEnum.valueOf()` and
 * `SubEnum.values()`. We cannot use `RemoveUnreachablePass` to remove them
 * because `Class.isEnum()` and `Class.getEnumConstants()` which use these
 * methods are commonly used in (de)serialization libraries so that we need
 * Proguard rule to keep these methods. There are two reasons why we wouldn't be
 * able to remove them.
 * 1. The method is called directly, e.g., `SubEnum.valueOf("ONE")`.
 * 2. The method is called reflectively, e.g., `Class.getDeclaredMethod("...")`
 *    and `Class.getEnumConstants()`.
 *
 * We can detect (2) by checking if the enum is ever upcasted to either an
 * `Object` type or an `Enum` type and if it escapes a method so that it becomes
 * hard to track. This could happen when an upcasted enum is returned from a
 * method, set to a class variable or array, or passed as an argument to a
 * method. We also need to check if the enum is ever used as the type
 * `java.lang.Class` by checking for `Class.getDeclaredClass()` and
 * `const-class`.
 */
class EnumAnalyzeGeneratedMethods {
 private:
  struct CandidateEnum {
    CandidateEnum(DexClass* cls,
                  const DexMethod* valueof_method,
                  const DexMethod* values_method)
        : cls(cls),
          valueof_method(valueof_method),
          values_method(values_method) {}

    DexClass* cls;
    const DexMethod* valueof_method;
    const DexMethod* values_method;
  };

 public:
  EnumAnalyzeGeneratedMethods()
      : config(/*max_enum_size*/ std::numeric_limits<uint32_t>::max()) {}

  /**
   * Adds the enum class and its generated methods to be considered for
   * optimization.
   */
  void consider_enum_type(DexClass* cls,
                          const DexMethod* valueof_method,
                          const DexMethod* values_method) {
    config.candidate_enums.insert(cls->get_type());
    m_candidate_enums.emplace_back(cls, valueof_method, values_method);
  }

  /**
   * Returns the number of enum generated methods that are candidates for
   * deletion.
   */
  size_t num_candidate_enum_methods() { return m_candidate_enums.size(); }

  /**
   * Finds which of generated methods of the considered enums are safe to remove
   * and removes them. Returns the number of methods that were removed.
   */
  size_t transform_code(const Scope& scope);

 private:
  Config config;
  std::vector<CandidateEnum> m_candidate_enums;
  ConcurrentSet<const DexMethod*> m_rejected_methods;
  ConcurrentSet<const DexType*> m_rejected_types;
};
} // namespace optimize_enums
