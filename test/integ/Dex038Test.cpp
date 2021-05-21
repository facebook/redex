/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

#include "DexCallSite.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "RedexTestUtils.h"
#include "SanitizersConfig.h"
#include "Show.h"
#include "Walkers.h"

using CallSitePredicate = const std::function<bool(DexCallSite*)>&;
int ensureCallSite(DexIdx* idx, CallSitePredicate predicate) {
  for (int i = 0; i < idx->get_callsite_ids_size(); ++i) {
    auto cs = idx->get_callsiteidx(i);
    if (predicate(cs)) return i;
  }
  return -1;
}

using MethodHandlePredicate = const std::function<bool(DexMethodHandle*)>&;
int ensureMethodHandle(DexIdx* idx, MethodHandlePredicate predicate) {
  for (int i = 0; i < idx->get_methodhandle_ids_size(); ++i) {
    auto mh = idx->get_methodhandleidx(i);
    if (predicate(mh)) return i;
  }
  return -1;
}

static const char* DEX038_CLASS_NAME = "Lcom/facebook/redextest/Dex038;";
static const char* SUPPLIER_CLASS_NAME = "Ljava/util/function/Supplier;";
static const char* STRING_CLASS_NAME = "Ljava/lang/String;";
static const char* VOID_RETURN_OBJECT_PROTO = "()Ljava/lang/Object;";
static const char* VOID_RETURN_STRING_PROTO = "()Ljava/lang/String;";

void testReadDex(const char* dexfile) {
  DexLoader dl(dexfile);
  dex_stats_t stats{{0}};
  auto classes = dl.load_dex(dexfile, &stats, 38);
  auto idx = dl.get_idx();

  EXPECT_EQ(idx->get_callsite_ids_size(), 7);
  EXPECT_EQ(idx->get_methodhandle_ids_size(), 8);

  // clang-format off

  // !!! N.B. !!! right now these tests assume a reliable ordering of
  // callsite/methodhandles by the dexer. it's definitely fragile.

  // verify lambda metafactory method handle shared by every callsite

  //   type        : invoke-static
  //   target      : Ljava/lang/invoke/LambdaMetafactory; metafactory
  //   target_type : (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;
  int metafactoryMethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_STATIC &&
      !strcmp(mh->methodref()->get_name()->c_str(), "metafactory") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), "Ljava/lang/invoke/LambdaMetafactory;") &&
      !strcmp(SHOW(mh->methodref()->get_proto()), "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;");
  });
  EXPECT_NE(metafactoryMethodHandleIdx, -1);
  DexMethodHandle* metafactoryMethodHandle = idx->get_methodhandleidx(metafactoryMethodHandleIdx);

  // verify all other method handles

  //   type        : invoke-static
  //   target      : Lcom/facebook/redextest/Dex038; lambda$run$0
  //   target_type : ()Ljava/lang/String;
  int lambdaRun0MethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_STATIC &&
      !strcmp(mh->methodref()->get_name()->c_str(), "lambda$run$0") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  });
  EXPECT_NE(lambdaRun0MethodHandleIdx, -1);
  DexMethodHandle* lambdaRun0MethodHandle = idx->get_methodhandleidx(lambdaRun0MethodHandleIdx);

  //   type        : invoke-static
  //   target      : Lcom/facebook/redextest/Dex038; lambda$run$1
  //   target_type : ()Ljava/lang/String;
  int lambdaRun1MethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_STATIC &&
      !strcmp(mh->methodref()->get_name()->c_str(), "lambda$run$1") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  });
  EXPECT_NE(lambdaRun1MethodHandleIdx, -1);
  DexMethodHandle* lambdaRun1MethodHandle = idx->get_methodhandleidx(lambdaRun1MethodHandleIdx);

  //   type        : invoke-static
  //   target      : Lcom/facebook/redextest/Dex038; lambda$run$2
  //   target_type : ()Ljava/lang/String;
  int lambdaRun2MethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type(), METHOD_HANDLE_TYPE_INVOKE_STATIC &&
      !strcmp(mh->methodref()->get_name()->c_str(), "lambda$run$2") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  });
  EXPECT_NE(lambdaRun2MethodHandleIdx, -1);
  DexMethodHandle* lambdaRun2MethodHandle = idx->get_methodhandleidx(lambdaRun2MethodHandleIdx);

  //   type        : invoke-static
  //   target      : Lcom/facebook/redextest/Dex038; staticStringSupplier
  //   target_type : ()Ljava/lang/String;
  int staticStringSupplierMethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_STATIC &&
      !strcmp(mh->methodref()->get_name()->c_str(), "staticStringSupplier") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  });
  EXPECT_NE(staticStringSupplierMethodHandleIdx, -1);
  DexMethodHandle* staticStringSupplierMethodHandle = idx->get_methodhandleidx(staticStringSupplierMethodHandleIdx);

  //   type        : invoke-instance
  //   target      : Lcom/facebook/redextest/Dex038; instanceStringSupplier
  //   target_type : (Lcom/facebook/redextest/Dex038;)Ljava/lang/String;
  int instanceStringSupplierMethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_INSTANCE &&
      !strcmp(mh->methodref()->get_name()->c_str(), "instanceStringSupplier") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  });
  EXPECT_NE(instanceStringSupplierMethodHandleIdx, -1);
  DexMethodHandle* instanceStringSupplierMethodHandle = idx->get_methodhandleidx(instanceStringSupplierMethodHandleIdx);

  //   type        : invoke-constructor
  //   target      : Ljava/lang/String; <init>
  //   target_type : (Ljava/lang/String;)V
  int constructorMethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_CONSTRUCTOR &&
      !strcmp(mh->methodref()->get_name()->c_str(), "<init>") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), STRING_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), "()V");
  });
  EXPECT_NE(constructorMethodHandleIdx, -1);
  DexMethodHandle* constructorMethodHandle = idx->get_methodhandleidx(constructorMethodHandleIdx);

  //   type        : invoke-direct
  //   target      : Lcom/facebook/redextest/Dex038; privateInstanceStringSupplier
  //   target_type : (Lcom/facebook/redextest/Dex038;)Ljava/lang/String;
  int directMethodHandleIdx = ensureMethodHandle(idx, [](DexMethodHandle* mh) {
    return
      mh->type() == METHOD_HANDLE_TYPE_INVOKE_DIRECT &&
      !strcmp(mh->methodref()->get_name()->c_str(), "privateInstanceStringSupplier") &&
      !strcmp(mh->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME) &&
      !strcmp(SHOW(mh->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  });
  EXPECT_NE(directMethodHandleIdx, -1);
  DexMethodHandle* directMethodHandle = idx->get_methodhandleidx(directMethodHandleIdx);

  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : (Lcom/facebook/redextest/Dex038;)Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), DEX038_CLASS_NAME) &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == instanceStringSupplierMethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // Call Site #1 // offset 2179
  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : (Lcom/facebook/redextest/Dex038;)Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), DEX038_CLASS_NAME) &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == directMethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // Call Site #2 // offset 2192
  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), "") &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == constructorMethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // Call Site #3 // offset 2205
  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), "") &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == staticStringSupplierMethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // Call Site #4 // offset 2218
  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), "") &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == lambdaRun0MethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // Call Site #5 // offset 2231
  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), "") &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == lambdaRun1MethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // Call Site #6 // offset 2244
  //   link_argument[0] : N (MethodHandle)
  //   link_argument[1] : get (String)
  //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
  //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
  //   link_argument[4] : N (MethodHandle)
  //   link_argument[5] : ()Ljava/lang/String; (MethodType)
  EXPECT_NE(ensureCallSite(idx, [&](DexCallSite* cs) {
    return
      cs->method_handle() == metafactoryMethodHandle &&
      !strcmp(cs->method_name()->c_str(), "get") &&
      !strcmp(cs->method_type()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME) &&
      !strcmp(SHOW(cs->method_type()->get_args()), "") &&
      cs->args().size() == 3 &&
      !strcmp(SHOW(cs->args()[0]), VOID_RETURN_OBJECT_PROTO) &&
      ((DexEncodedValueMethodHandle*)cs->args()[1])->methodhandle() == lambdaRun2MethodHandle &&
      !strcmp(SHOW(cs->args()[2]), VOID_RETURN_STRING_PROTO);
  }), -1);

  // clang-format on
}

TEST(Dex038Test, ReadDex038) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  EXPECT_NE(nullptr, dexfile);

  testReadDex(dexfile);
}

TEST(Dex038Test, ReadWriteDex038) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  EXPECT_NE(nullptr, dexfile);

  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile, true, 38));
  DexClasses& classes = root_store.get_dexen().back();
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(root_store));

  //  DexClasses classes = load_classes_from_dex(dexfile, true, 38);
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(""));
  std::unordered_map<DexMethod*, uint64_t> method_to_id;
  std::unordered_map<DexCode*, std::vector<DebugLineItem>> code_debug_lines;

  Json::Value conf_obj = Json::nullValue;
  auto tmpdir = redex::make_tmp_dir("dex038_test_%%%%%%%%");
  ConfigFiles dummy_cfg(conf_obj, tmpdir.path);
  RedexOptions dummy_options;

  std::string metafiles = tmpdir.path + "/meta";
  int status = mkdir(metafiles.c_str(), 0755);
  if (status) {
    EXPECT_EQ(EEXIST, errno);
  }

  instruction_lowering::run(stores, true);

  std::string output_dex = tmpdir.path + "/output.dex";
  write_classes_to_dex(dummy_options,
                       output_dex,
                       &classes,
                       nullptr,
                       0,
                       0,
                       dummy_cfg,
                       pos_mapper.get(),
                       &method_to_id,
                       &code_debug_lines,
                       nullptr,
                       "dex\n038\0");

  delete g_redex;
  g_redex = new RedexContext();

  testReadDex(output_dex.c_str());

  delete g_redex;
}
