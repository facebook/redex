/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IPConstantPropagation.h"

#include <algorithm>

#include <gtest/gtest.h>

#include "ConfigFiles.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "TypeUtil.h"
#include "VirtualScope.h"

using namespace constant_propagation;
using namespace constant_propagation::interprocedural;

namespace {

void create_external_class_once(DexType* type,
                                const std::vector<const char*>& method_specs) {
  if (type_class(type) != nullptr) {
    return;
  }
  ClassCreator cc(type);
  cc.set_super(type::java_lang_Object());
  cc.set_external();
  for (const auto* spec : method_specs) {
    auto* ref = DexMethod::make_method(spec);
    bool is_ctor = ref->get_name()->str() == "<init>";
    cc.add_method(ref->make_concrete(is_ctor ? (ACC_PUBLIC | ACC_CONSTRUCTOR)
                                             : ACC_PUBLIC,
                                     /* is_virtual */ !is_ctor));
  }
  cc.create();
}

// The builder-state analysis and the reduction resolve java.lang.StringBuilder
// and java.lang.String methods, so both must exist as (external) classes.
void prepare_string_classes() {
  create_external_class_once(
      type::java_lang_String(),
      {"Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;"});
  create_external_class_once(
      DexType::make_type("Ljava/lang/StringBuilder;"),
      {"Ljava/lang/StringBuilder;.<init>:()V",
       // The builder-state analysis asserts this String-arg constructor exists.
       "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V",
       "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
       "StringBuilder;",
       "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;"});
}

bool has_string_concat(DexMethod* method) {
  const auto* concat_ref = DexMethod::get_method(
      "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;");
  auto ii = InstructionIterable(method->get_code());
  return std::any_of(
      ii.begin(), ii.end(), [concat_ref](const MethodItemEntry& mie) {
        return mie.insn->has_method() && mie.insn->get_method() == concat_ref;
      });
}

} // namespace

struct IPCPStringBuilderAppendChainTest : public RedexTest {
 public:
  IPCPStringBuilderAppendChainTest() {
    // Mirrors InterproceduralConstantPropagationTest: get_vmethods initializes
    // the object class, which is needed to build a proper scope.
    virt_scope::get_vmethods(type::java_lang_Object());
    auto* object_ctor = method::java_lang_Object_ctor();
    object_ctor->set_access(ACC_PUBLIC | ACC_CONSTRUCTOR);
    object_ctor->set_external();
    type_class(type::java_lang_Object())->add_method(object_ctor);
    type_class(type::java_lang_Object())->set_external();
    prepare_string_classes();
  }

 protected:
  struct CallersAndCallee {
    DexMethod* const_string_caller;
    DexMethod* nullable_caller;
    DexMethod* callee;
  };

  // Builds this in `scope`:
  //
  //   constStringCaller() { return callee("a", "b"); }
  //   nullableCaller(String p) { return callee("a", p); }
  //   callee(String p0, String p1) {
  //     return new StringBuilder().append(p0).append(p1).toString();
  //   }
  //
  // Whether p0 and p1 could be proven to be non-null depends on which caller is
  // the root.
  static CallersAndCallee create_callers_and_callee(Scope& scope) {
    ClassCreator creator(DexType::make_type("LFoo;"));
    creator.set_super(type::java_lang_Object());

    auto* const_string_caller = assembler::method_from_string(R"(
      (method (public static) "LFoo;.constStringCaller:()Ljava/lang/String;"
       (
        (const-string "a")
        (move-result-pseudo-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (invoke-static (v0 v1) "LFoo;.callee:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
       )
      )
    )");
    creator.add_method(const_string_caller);
    const_string_caller->get_code()->build_cfg();

    auto* nullable_caller = assembler::method_from_string(R"(
      (method (public static) "LFoo;.nullableCaller:(Ljava/lang/String;)Ljava/lang/String;"
       (
        (load-param-object v0)
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-static (v1 v0) "LFoo;.callee:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
       )
      )
    )");
    creator.add_method(nullable_caller);
    nullable_caller->get_code()->build_cfg();

    auto* callee = assembler::method_from_string(R"(
      (method (private static) "LFoo;.callee:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
       (
        (load-param-object v0)
        (load-param-object v1)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v2)
        (invoke-direct (v2) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v2 v0) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v2 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v2) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
       )
      )
    )");
    creator.add_method(callee);
    callee->get_code()->build_cfg();
    scope.push_back(creator.create());
    return {const_string_caller, nullable_caller, callee};
  }

  // Drives the pass through PassManager, the entry point that reads whether
  // InterDex has run, reporting it as run or not. PassManager parses each
  // pass's options from the JSON config, so the reduction is enabled there
  // rather than through a Config passed to the constructor.
  static void run_through_pass_manager(
      const Scope& scope,
      bool interdex_has_run,
      const char* option = "reduce_stringbuilder_concat") {
    Json::Value pass_config(Json::objectValue);
    pass_config[option] = true;
    pass_config["max_heap_analysis_iterations"] = 1;
    Json::Value json_config(Json::objectValue);
    json_config["InterproceduralConstantPropagationPass"] = pass_config;

    ConfigFiles pass_conf(json_config);
    pass_conf.parse_global_config();

    InterproceduralConstantPropagationPass pass;
    std::vector<Pass*> passes{&pass};
    PassManager manager(passes, pass_conf);
    if (interdex_has_run) {
      manager.record_running_interdex();
    }
    auto store = DexStore("store");
    store.add_classes(scope);
    DexStoresVector stores({store});
    manager.run_passes(stores, pass_conf);
  }

  static bool code_has_string_concat(DexMethod* method) {
    if (method->get_code()->cfg_built()) {
      method->get_code()->clear_cfg();
    }
    return has_string_concat(method);
  }
};

/*
 * With only the const-string caller reachable, IPCP's whole-program state
 * proves both of the callee's parameters non-null and the reduction fires.
 */
TEST_F(IPCPStringBuilderAppendChainTest, interproceduralNonNullParamsReduced) {
  Scope scope;
  auto methods = create_callers_and_callee(scope);
  methods.const_string_caller->rstate.set_root();

  run_through_pass_manager(scope, /* interdex_has_run */ false);

  EXPECT_TRUE(code_has_string_concat(methods.callee));
}

/*
 * The reduction is skipped when the PassManager reports InterDex as already
 * run.
 */
TEST_F(IPCPStringBuilderAppendChainTest, notReducedAfterInterDex) {
  Scope scope;
  auto methods = create_callers_and_callee(scope);
  methods.const_string_caller->rstate.set_root();

  run_through_pass_manager(scope, /* interdex_has_run */ true);

  EXPECT_FALSE(code_has_string_concat(methods.callee));
}

/*
 * With only the nullable caller reachable, the callee's second operand is that
 * caller's own parameter, which IPCP cannot prove non-null, so the builder is
 * left intact.
 */
TEST_F(IPCPStringBuilderAppendChainTest,
       nullableInterproceduralParamNotReduced) {
  Scope scope;
  auto methods = create_callers_and_callee(scope);
  methods.nullable_caller->rstate.set_root();

  run_through_pass_manager(scope, /* interdex_has_run */ false);

  EXPECT_FALSE(code_has_string_concat(methods.callee));
}

/*
 * A builder holding one constant append is replaced by the string it produces,
 * so its toString() is gone once the pass runs with
 * replace_constant_stringbuilder_tostring_with_const_string enabled.
 */
TEST_F(IPCPStringBuilderAppendChainTest, constantBuilderReplacedThroughPass) {
  Scope scope;
  ClassCreator creator(DexType::make_type("LBar;"));
  creator.set_super(type::java_lang_Object());
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LBar;.f:()Ljava/lang/String;"
     (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "a")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
     )
    )
  )");
  creator.add_method(method);
  method->get_code()->build_cfg();
  method->rstate.set_root();
  scope.push_back(creator.create());

  run_through_pass_manager(
      scope, /* interdex_has_run */ false,
      "replace_constant_stringbuilder_tostring_with_const_string");

  method->get_code()->clear_cfg();
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "a")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "a")
      (move-result-pseudo-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}
