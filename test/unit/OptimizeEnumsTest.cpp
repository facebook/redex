/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional/optional_io.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "EnumConfig.h"
#include "IRAssembler.h"
#include "OptimizeEnums.h"
#include "RedexTest.h"
#include "TypeUtil.h"

using namespace testing;

class OptimizeEnumsTest : public RedexTest {};

optimize_enums::ParamSummary get_summary(const std::string& s_expr) {
  auto* method = assembler::method_from_string(s_expr);
  method->get_code()->build_cfg();
  return optimize_enums::calculate_param_summary(method,
                                                 type::java_lang_Object());
}

TEST_F(OptimizeEnumsTest, test_param_summary_generating) {
  auto summary = get_summary(R"(
    (method (static) "LFoo;.upcast_when_return:(Ljava/lang/Enum;)Ljava/lang/Object;"
      (
        (load-param-object v0)
        (return-object v0)
      )
    )
  )");
  EXPECT_EQ(summary.returned_param, std::nullopt);
  EXPECT_TRUE(summary.safe_params.empty());

  auto summary2 = get_summary(R"(
    (method (public) "LFoo;.param_0_is_not_safecast:(Ljava/lang/Enum;Ljava/lang/Object;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (load-param-object v2)
        (return-void)
      )
    )
  )");
  EXPECT_EQ(summary2.returned_param, std::nullopt);
  EXPECT_THAT(unordered_unsafe_unwrap(summary2.safe_params),
              UnorderedElementsAre(2));

  auto summary2_static = get_summary(R"(
    (method (static public) "LFoo;.param_0_is_not_safecast:(Ljava/lang/Enum;Ljava/lang/Object;)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (return-void)
      )
    )
  )");
  EXPECT_EQ(summary2_static.returned_param, std::nullopt);
  EXPECT_THAT(unordered_unsafe_unwrap(summary2_static.safe_params),
              UnorderedElementsAre(1));

  auto summary3 = get_summary(R"(
    (method () "LFoo;.check_cast:(Ljava/lang/Object;)Ljava/lang/Object;"
      (
        (load-param-object v1)
        (load-param-object v0)
        (check-cast v0 "Ljava/lang/Enum;")
        (move-result-pseudo-object v0)
        (return-object v0)
      )
    )
  )");
  EXPECT_EQ(summary3.returned_param, std::nullopt);
  EXPECT_TRUE(summary3.safe_params.empty());

  auto summary4 = get_summary(R"(
    (method () "LFoo;.has_invocation:(Ljava/lang/Object;)Ljava/lang/Object;"
      (
        (load-param-object v1)
        (load-param-object v0)
        (invoke-virtual (v0) "Ljava/lang/Object;.toString:()Ljava/lang/String;")
        (return-object v0)
      )
    )
  )");
  EXPECT_EQ(summary4.returned_param, std::nullopt);
  EXPECT_TRUE(summary4.safe_params.empty());
}

/* Create a Bar enum class and a shell implementations of Enum, Integer, Object
class enum Bar {
  Y(2132351567),
  Z(2132351568);

  @IdRes int x;
  Bar(int x) { this.x = x; }

  int fromInt(int i) {
    if (i == 0) {
      return Y.x;
    } else {
      return Z.x;
    }
  }
}
*/
DexStore create_enum_store() {
  // Make a fake Enum class that Bar can call
  ClassCreator ec(type::java_lang_Enum());
  ec.add_method(assembler::method_from_string(R"(
    (method (public constructor) "Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (load-param v2)
        (return-void)
      )
    )
  )"));
  ec.add_method(assembler::method_from_string(R"(
    (method (public constructor) "Ljava/lang/Enum;.ordinal:()I"
      (
        (load-param-object v0)
        (const v1 0)
        (return v1)
      )
    )
  )"));
  ec.add_method(assembler::method_from_string(R"(
    (method (public) "Ljava/lang/Enum;.getDeclaringClass:()Ljava/lang/Class;"
      (
        (load-param-object v0)
        (const v1 0)
        (return-object v1)
      )
    )
  )"));
  ec.set_super(type::java_lang_Object());
  ec.set_access(ACC_PUBLIC);
  auto* enums = ec.create();

  ClassCreator ic(type::java_lang_Integer());
  ic.add_method(assembler::method_from_string(R"(
    (method (public) "Ljava/lang/Integer;.intValue:()I"
      (
        (load-param-object v0)
        (const v1 0)
        (return v1)
      )
    )
  )"));
  ic.set_super(type::java_lang_Object());
  ic.set_access(ACC_PUBLIC);
  auto* ints = ic.create();

  ClassCreator oc(type::java_lang_Object());
  oc.add_method(assembler::method_from_string(R"(
    (method (public) "Ljava/lang/Object;.getClass:()Ljava/lang/Class;"
      (
        (load-param-object v0)
        (const v1 0)
        (return v1)
      )
    )
  )"));
  oc.set_access(ACC_PUBLIC);
  auto* objs = oc.create();

  ClassCreator cc(DexType::make_type("LBar;"));
  cc.set_super(type::java_lang_Enum());
  cc.set_access(ACC_PUBLIC | ACC_FINAL | ACC_ENUM);

  // Add instance field
  auto* x = dynamic_cast<DexField*>(DexField::make_field("LBar;.x:I"));
  x->make_concrete(ACC_PUBLIC);
  cc.add_field(x);

  // Add init
  cc.add_method(assembler::method_from_string(R"(
    (method (private constructor) "LBar;.<init>:(Ljava/lang/String;II)V"
      (
        (load-param-object v0)
        (load-param-object v1)
        (load-param v2)
        (load-param v3)
        (invoke-direct (v0 v1 v2) "Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V")
        (iput v3 v0 "LBar;.x:I")
        (return-void)
      )
    )
  )"));

  // Add static enum fields
  auto* VALUES =
      dynamic_cast<DexField*>(DexField::make_field("LBar;.$VALUES:[LBar;"));
  VALUES->make_concrete(ACC_STATIC | ACC_FINAL | ACC_SYNTHETIC);
  cc.add_field(VALUES);
  auto* Y = dynamic_cast<DexField*>(DexField::make_field("LBar;.Y:LBar;"));
  Y->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_ENUM);
  cc.add_field(Y);
  auto* Z = dynamic_cast<DexField*>(DexField::make_field("LBar;.Z:LBar;"));
  Z->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_ENUM);
  cc.add_field(Z);

  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LBar;.fromInt:(I)I"
     (
      (load-param v0)
      (if-eqz v0 :L1)
      (sget-object "LBar;.Y:LBar;")
      (move-result-pseudo-object v1)
      (iget v1 "LBar;.x:I")
      (move-result-pseudo v1)
      (return v1)
      (:L1)
      (sget-object "LBar;.Z:LBar;")
      (move-result-pseudo-object v1)
      (iget v1 "LBar;.x:I")
      (move-result-pseudo v1)
      (return v1)
     )
    )
  )"));

  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LBar;.$values:()[LBar;"
     (
      (const v0 2)
      (new-array v0 "[LBar;")
      (move-result-pseudo-object v0)
      (const v1 0)
      (sget-object "LBar;.Y:LBar;")
      (move-result-pseudo-object v2)
      (aput-object v2 v0 v1)
      (const v1 1)
      (sget-object "LBar;.Z:LBar;")
      (move-result-pseudo-object v2)
      (aput-object v2 v0 v1)
      (return-object v0)
     )
    )
  )"));

  // Add clinit. This part contains the unique pattern to test, which is when an
  // instance field is a resource ID specified using the R_CONST instruction.
  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LBar;.<clinit>:()V"
     (
      (r-const v0 2132351567)
      (const-string "Y")
      (move-result-pseudo-object v1)
      (const v2 0)
      (new-instance "LBar;")
      (move-result-pseudo-object v3)
      (invoke-direct (v3 v1 v2 v0) "LBar;.<init>:(Ljava/lang/String;II)V")
      (sput-object v3 "LBar;.Y:LBar;")
      (r-const v0 2132351568)
      (const-string "Z")
      (move-result-pseudo-object v1)
      (const v2 1)
      (new-instance "LBar;")
      (move-result-pseudo-object v3)
      (invoke-direct (v3 v1 v2 v0) "LBar;.<init>:(Ljava/lang/String;II)V")
      (sput-object v3 "LBar;.Z:LBar;")
      (invoke-static () "LBar;.$values:()[LBar;")
      (move-result-object v0)
      (sput-object v0 "LBar;.$VALUES:[LBar;")
      (return-void)
     )
    )
  )"));
  auto* bar = cc.create();

  for (auto* m : bar->get_all_methods()) {
    m->get_code()->build_cfg();
  }

  auto store = DexStore("classes");
  store.add_classes({bar, ints, enums, objs});
  return store;
}

TEST_F(OptimizeEnumsTest, analyzeRConstMembers) {
  DexStoresVector stores({create_enum_store()});
  auto scope = build_class_scope(stores);
  auto* bar = scope[0];

  optimize_enums::OptimizeEnumsPass pass;
  PassManager manager({&pass});
  optimize_enums::Config config(100, false, false, {});
  config.candidate_enums.insert(bar->get_type());

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_config(conf_obj);
  dummy_config.parse_global_config();

  manager.run_passes(stores, dummy_config);

  auto* get_x =
      bar->find_method_from_simple_deobfuscated_name("redex$OE$get_x");
  EXPECT_NE(get_x, nullptr);
  get_x->get_code()->build_cfg();

  auto& cfg = get_x->get_code()->cfg();
  int const_count = 0;
  int rconst_count = 0;
  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    if (it->insn->opcode() == OPCODE_CONST) {
      const_count++;
    } else if (it->insn->opcode() == IOPCODE_R_CONST) {
      rconst_count++;
    }
  }
  EXPECT_EQ(const_count, 0);
  EXPECT_EQ(rconst_count, 2);
}
