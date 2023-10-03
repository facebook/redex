/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "Pass.h"

class PrintKotlinStats : public Pass {

 public:
  struct Stats {
    size_t unknown_null_check_insns{0};
    size_t kotlin_null_check_insns{0};
    size_t java_public_param_objects{0};
    size_t kotlin_public_param_objects{0};
    size_t kotlin_delegates{0};
    size_t kotlin_lazy_delegates{0};
    size_t kotlin_lambdas{0};
    size_t kotlin_non_capturing_lambda{0};
    size_t kotlin_class_with_instance{0};
    size_t kotlin_class{0};
    size_t kotlin_anonymous_class{0};
    size_t kotlin_companion_class{0};
    size_t di_generated_class{0};
    size_t kotlin_default_arg_method{0};
    size_t kotlin_coroutine_continuation_base{0};

    Stats& operator+=(const Stats& that) {
      unknown_null_check_insns += that.unknown_null_check_insns;
      kotlin_null_check_insns += that.kotlin_null_check_insns;
      java_public_param_objects += that.java_public_param_objects;
      kotlin_public_param_objects += that.kotlin_public_param_objects;
      kotlin_delegates += that.kotlin_delegates;
      kotlin_lazy_delegates += that.kotlin_lazy_delegates;
      kotlin_lambdas += that.kotlin_lambdas;
      kotlin_non_capturing_lambda += that.kotlin_non_capturing_lambda;
      kotlin_class_with_instance += that.kotlin_class_with_instance;
      kotlin_class += that.kotlin_class;
      kotlin_anonymous_class += that.kotlin_anonymous_class;
      kotlin_companion_class += that.kotlin_companion_class;
      di_generated_class += that.di_generated_class;
      kotlin_default_arg_method += that.kotlin_default_arg_method;
      kotlin_coroutine_continuation_base +=
          that.kotlin_coroutine_continuation_base;
      return *this;
    }

    /// Updates metrics tracked by \p mgr corresponding to these statistics.
    void report(PassManager& mgr) const;
  };

  PrintKotlinStats() : Pass("PrintKotlinStatsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  void setup();
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  Stats handle_method(DexMethod* method);
  Stats handle_class(DexClass* cls);
  Stats get_stats() { return m_stats; }

 private:
  std::unordered_set<DexMethodRef*> m_kotlin_null_assertions;
  DexType* m_kotlin_lambdas_base = nullptr;
  DexType* m_kotlin_coroutin_continuation_base = nullptr;
  const DexString* m_instance = nullptr;
  DexType* m_di_base = nullptr;
  Stats m_stats;
};
