/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InstructionSequenceOutliner.h"

#include <gtest/gtest.h>
#include <iterator>
#include <utility>

#include "ControlFlow.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "ScopeHelper.h"

namespace {

void run_passes(const std::vector<Pass*>& passes,
                std::vector<DexClass*> classes) {
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore store(dm);
  // primary dex
  store.add_classes({});
  // secondary dex
  store.add_classes(std::move(classes));
  stores.emplace_back(std::move(store));
  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_config(conf_obj);
  dummy_config.parse_global_config();
  manager.run_passes(stores, dummy_config);
}

struct InstructionSequenceOutlinerTest : public RedexTest {
  std::vector<DexClass*> m_classes;
  DexMethod* m_init;
  DexMethod* m_other;

  void create() {
    std::vector<DexType*> no_interfaces;

    DexType* c_type = DexType::make_type("LC;");
    DexClass* c_cls = create_class(c_type, type::java_lang_Object(),
                                   no_interfaces, ACC_PUBLIC);
    m_classes.push_back(c_cls);

    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(type::_void(), args);

    m_init =
        DexMethod::make_method(c_type, DexString::make_string("<init>"), proto)
            ->make_concrete(ACC_PUBLIC, false);
    m_init->set_code(std::make_unique<IRCode>(m_init, 1));
    c_cls->add_method(m_init);

    m_other =
        DexMethod::make_method(c_type, DexString::make_string("other"), proto)
            ->make_concrete(ACC_PUBLIC, false);
    m_other->set_code(std::make_unique<IRCode>(m_other, 1));
    c_cls->add_method(m_other);

    auto field = DexField::make_field("LC;.f:Ljava/lang/Object;")
                     ->make_concrete(ACC_PUBLIC);
    c_cls->add_field(field);
  }

  void run() {
    std::vector<Pass*> passes = {new InstructionSequenceOutliner()};
    run_passes(passes, m_classes);
  }

  ~InstructionSequenceOutlinerTest() {}
};

} // namespace

// Tests that we can outline iputs that operate on the new instance in a
// constructor after the base constructor call, but not before. We use IR as we
// cannot write this test in Java or Kotlin directly, due to language
// limitations, even though such iputs are perfectly fine according to the JVM
// and Dalvik spec, and can arise in similar multiples due to other Redex
// optimizations such as ClassMerging.
TEST_F(InstructionSequenceOutlinerTest, iputsBeforeBaseInitInvocation) {
  using namespace dex_asm;
  create();

  auto init_str = R"(
    (
      (load-param-object v0)
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (return-void)
    )
  )";

  auto other_str = R"(
    (
      (load-param-object v0)
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (return-void)
    )
  )";

  auto init_code = assembler::ircode_from_string(init_str);
  m_init->set_code(std::move(init_code));

  auto other_code = assembler::ircode_from_string(other_str);
  m_other->set_code(std::move(other_code));

  run();

  auto expected_init_str = R"(
    (
      (load-param-object v0)
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (iput-object v0 v0 "LC;.f:Ljava/lang/Object;")
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (invoke-static (v0) "LC;.$outlined$0$45c530c69cb11355:(LC;)V")
      (invoke-static (v0) "LC;.$outlined$0$45c530c69cb11355:(LC;)V")
      (return-void)
    )
  )";

  auto expected_init_code = assembler::ircode_from_string(expected_init_str);
  auto outlined_init_code = m_init->get_code();
  // remove positions, we are not testing that here
  for (auto it = outlined_init_code->begin();
       it != outlined_init_code->end();) {
    if (it->type == MFLOW_POSITION) {
      it = outlined_init_code->erase(it);
    } else {
      it++;
    }
  }
  EXPECT_CODE_EQ(expected_init_code.get(), outlined_init_code);
}

// Tests that we do not create a clinit cycle.
// NOTE: This is only an initial test for a small workaround. Proper
//       work to detect cycles (and a better test) are future work.
TEST_F(InstructionSequenceOutlinerTest, doNotCreateClinitCycle) {
  using namespace dex_asm;

  // Setup:
  //
  // class Foo {
  //   public Foo(int i, int j, int k) {}
  //   public Foo(Foo other) {}
  // }
  //
  // class A {
  //   static Foo foo1 = new Foo(1, 1+1, 2+1);
  //   static Foo foo2 = new Foo(2, 2+1, 3+1);
  //   static Foo foo3 = new Foo(3, 3+1, 4+1);
  //   ...
  // }
  //
  // class B extends A {
  //   static Foo foo1 = new Foo(1, 1+1, 2+1);
  //   static Foo foo2 = new Foo(1, 1+1, 2+1);
  //   static Foo foo3 = new Foo(1, 1+1, 2+1);
  //
  //   static Foo loop = new Foo(A.foo1);
  // }

  using namespace assembler;

  auto foo_cls = assembler::class_from_string(R"(
    (class (public final) "LFoo;"
      (method (public) "LFoo;.<init>:(III)V"
        (
          (load-param-object v0) (load-param v1) (load-param v2) (load-param v3)
          (return-void)
        )
      )
      (method (public) "LFoo;.<init>:(LFoo;)V"
        (
          (load-param-object v0) (load-param-object v1)
          (return-void)
        )
      )
    )
  )");
  m_classes.push_back(foo_cls);

  auto field_str = [](std::string_view class_name, size_t idx) {
    return std::string("(field (public static) \"") + class_name + ".foo" +
           std::to_string(idx) + ":LFoo;\")";
  };

  auto init_fooX_str = [](const char* class_name, size_t idx) {
    char buf[4096]; // Definitely large enough.
    snprintf(buf, 4096, R"(
      (const v0 %zu)
      (add-int/lit v1 v0 1)
      (add-int/lit v2 v1 1)
      (new-instance "LFoo;")
      (move-result-pseudo-object v3)
      (invoke-direct (v3 v0 v1 v2) "LFoo;.<init>:(III)V")
      (sput-object v3 "%s.foo%zu:LFoo;")
    )",
             idx, class_name, idx);
    return std::string(buf);
  };

  constexpr size_t kFields = 10;

  auto a_cls = assembler::class_from_string(
      "(class (public final) \"LA;\" " +
      [&]() {
        std::string tmp;
        for (size_t i = 0; i < kFields; ++i) {
          tmp += field_str("LA;", i);
        }
        return tmp;
      }() +
      "(method (public static) \"LA;.<clinit>:()V\" (" +
      [&]() {
        std::string tmp;
        for (size_t i = 0; i < kFields; ++i) {
          tmp += init_fooX_str("LA;", i);
        }
        return tmp;
      }() +
      "(return-void) )))");
  m_classes.push_back(a_cls);
  EXPECT_EQ(a_cls, type_class(a_cls->get_type()));

  auto b_cls = assembler::class_from_string(
      "(class (public final) \"LB;\" extends \"LA;\" " +
      [&]() {
        std::string tmp;
        for (size_t i = 0; i < kFields + 1; ++i) {
          tmp += field_str("LB;", i);
        }
        return tmp;
      }() +
      "(method (public static) \"LB;.<clinit>:()V\" (" +
      [&]() {
        std::string tmp;
        for (size_t i = 1; i < kFields + 1; ++i) {
          tmp += init_fooX_str("LB;", i);
        }
        return tmp;
      }() +
      R"(
          (sget-object "LA;.foo0:LFoo;")
          (move-result-pseudo-object v0)
          (new-instance "LFoo;")
          (move-result-pseudo-object v1)
          (invoke-direct (v1 v0) "LFoo;.<init>:(LFoo;)V")
          (sput-object v3 "LB;.foo0:LFoo;")
          (return-void)
        )
      )))");
  m_classes.push_back(b_cls);
  EXPECT_EQ(b_cls, type_class(b_cls->get_type()));

  EXPECT_EQ(a_cls->get_dmethods().size(), 1);
  EXPECT_EQ(b_cls->get_dmethods().size(), 1);

  run();

  // Inserted outlined method into A.
  EXPECT_EQ(a_cls->get_dmethods().size(), 2);
  EXPECT_EQ(b_cls->get_dmethods().size(), 1);
}
