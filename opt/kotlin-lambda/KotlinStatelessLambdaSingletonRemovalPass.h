/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "KotlinLambdaAnalyzer.h"
#include "MethodProfiles.h"
#include "Pass.h"

class KotlinStatelessLambdaSingletonRemovalPass : public Pass {

 public:
  KotlinStatelessLambdaSingletonRemovalPass()
      : Pass("KotlinStatelessLambdaSingletonRemovalPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
Javac no longer generates anonymous classes or "inner classes" to desugar lambdas. It defers the generation of these classes until runtime via LambdaMetafactory.
Based on the [JDK doc](https://docs.oracle.com/en/java/javase/17/docs/api/java.base/java/lang/invoke/LambdaMetafactory.html), the identity of a produced function object at runtime by LambdaMetafactory is "unpredictable".

Kotlinc, since version 2.0 by default also stopped generating anonymous classes for lambdas. Similar to javac, it relies on invoke-dynamic and LambdaMetafactory to generate the classes at runtime on JVM.
However, this feature is not supported by Android runtime. To execute the same code on Android, D8 still needs to desugar lambdas back to an anonymous class.
Whiles D8 desugars stateless lambdas or "non-capturing lambdas", it does not scaldfold the singleton pattern anymore like it used to do (https://issuetracker.google.com/u/2/issues/222081665).
The rationale is that the singleton pattern was inherited from the old javac behavior which is optimized for JVM server workload aiming for high throughput rather than low latency.
On Android devices, peak performance throughput is less relevant than initial startup latency. Therefore, the singleton pattern is not no longer desirable.

This godbolt [example](https://godbolt.org/z/Mznrzs8T4) shows the singleton pattern produced by our current kotlinc setup. This pass removes the singleton pattern shown in the example.

This pass replaces references to the singleton `INSTANCE` field (via `sget-object`) with inline instantiation (`new-instance` + `move-result-pseudo-object` + `invoke-direct <init>`), and removes the static `INSTANCE` field and its initialization in `<clinit>`.
    )");
  }

  void bind_config() override {
    bind("exclude_hot",
         false,
         m_exclude_hot,
         "Exclude hot lambdas from singleton removal");
    bind("exclude_hot_call_count_threshold",
         kDefaultExcludeHotCallCountThreshold,
         m_exclude_hot_call_count_threshold,
         "Call count threshold for determining hot lambdas (used when "
         "exclude_hot is true)");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  static constexpr float kDefaultExcludeHotCallCountThreshold = 5.0f;

  bool is_hot_lambda(
      const KotlinLambdaAnalyzer& analyzer,
      const method_profiles::MethodProfiles& method_profiles) const;

  bool m_exclude_hot{false};
  float m_exclude_hot_call_count_threshold{
      kDefaultExcludeHotCallCountThreshold};
};
