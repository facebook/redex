/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

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
#include "Walkers.h"

void testReadDex(const char* dexfile) {
  DexLoader dl(dexfile);
  dex_stats_t stats{0};
  auto classes = dl.load_dex(dexfile, &stats, 38);
  auto idx = dl.get_idx();

  EXPECT_EQ(idx->get_callsite_ids_size(), 7);
  EXPECT_EQ(idx->get_methodhandle_ids_size(), 8);

  const char* DEX038_CLASS_NAME = "Lcom/facebook/redextest/Dex038;";
  const char* SUPPLIER_CLASS_NAME = "Ljava/util/function/Supplier;";
  const char* STRING_CLASS_NAME = "Ljava/lang/String;";
  const char* VOID_RETURN_OBJECT_PROTO = "()Ljava/lang/Object;";
  const char* VOID_RETURN_STRING_PROTO = "()Ljava/lang/String;";

  // clang-format off

  // !!! N.B. !!! right now these tests assume a reliable ordering of 
  // callsite/methodhandles by the dexer. it's definitely fragile.

  // verify lambda metafactory method handle shared by every callsite

  {
    // Method Handle #4:
    //   type        : invoke-static
    //   target      : Ljava/lang/invoke/LambdaMetafactory; metafactory
    //   target_type : (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;
    auto metafactoryMethodHandle = idx->get_methodhandleidx(4);
    EXPECT_EQ(metafactoryMethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_STATIC);
    EXPECT_STREQ(metafactoryMethodHandle->methodref()->get_name()->c_str(), "metafactory");
    EXPECT_STREQ(metafactoryMethodHandle->methodref()->get_class()->get_name()->c_str(), "Ljava/lang/invoke/LambdaMetafactory;");
    EXPECT_STREQ(SHOW(metafactoryMethodHandle->methodref()->get_proto()), "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;");
  }

  // verify all other method handles

  {
    // Method Handle #0:
    //   type        : invoke-static
    //   target      : Lcom/facebook/redextest/Dex038; lambda$run$0
    //   target_type : ()Ljava/lang/String; 
    auto invokeStatic$lambda$run$0MethodHandle = idx->get_methodhandleidx(0);
    EXPECT_EQ(invokeStatic$lambda$run$0MethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_STATIC);
    EXPECT_STREQ(invokeStatic$lambda$run$0MethodHandle->methodref()->get_name()->c_str(), "lambda$run$0");
    EXPECT_STREQ(invokeStatic$lambda$run$0MethodHandle->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$0MethodHandle->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  }

  {
    // Method Handle #1:
    //   type        : invoke-static
    //   target      : Lcom/facebook/redextest/Dex038; lambda$run$1
    //   target_type : ()Ljava/lang/String;    
    auto invokeStatic$lambda$run$1MethodHandle = idx->get_methodhandleidx(1);
    EXPECT_EQ(invokeStatic$lambda$run$1MethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_STATIC);
    EXPECT_STREQ(invokeStatic$lambda$run$1MethodHandle->methodref()->get_name()->c_str(), "lambda$run$1");
    EXPECT_STREQ(invokeStatic$lambda$run$1MethodHandle->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$1MethodHandle->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  }

  {
    // Method Handle #2:
    //   type        : invoke-static
    //   target      : Lcom/facebook/redextest/Dex038; lambda$run$2
    //   target_type : ()Ljava/lang/String;
    auto invokeStatic$lambda$run$2MethodHandle = idx->get_methodhandleidx(2);
    EXPECT_EQ(invokeStatic$lambda$run$2MethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_STATIC);
    EXPECT_STREQ(invokeStatic$lambda$run$2MethodHandle->methodref()->get_name()->c_str(), "lambda$run$2");
    EXPECT_STREQ(invokeStatic$lambda$run$2MethodHandle->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$2MethodHandle->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  }

  {
    // Method Handle #3:
    //   type        : invoke-static
    //   target      : Lcom/facebook/redextest/Dex038; staticStringSupplier
    //   target_type : ()Ljava/lang/String;    
    auto invokeStaticStringSupplierMethodHandle = idx->get_methodhandleidx(3);
    EXPECT_EQ(invokeStaticStringSupplierMethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_STATIC);
    EXPECT_STREQ(invokeStaticStringSupplierMethodHandle->methodref()->get_name()->c_str(), "staticStringSupplier");
    EXPECT_STREQ(invokeStaticStringSupplierMethodHandle->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStaticStringSupplierMethodHandle->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  }

  {
    // Method Handle #5:
    //   type        : invoke-instance
    //   target      : Lcom/facebook/redextest/Dex038; instanceStringSupplier
    //   target_type : (Lcom/facebook/redextest/Dex038;)Ljava/lang/String;
    auto invokeInstanceStringSupplierMethodHandle = idx->get_methodhandleidx(5);
    EXPECT_EQ(invokeInstanceStringSupplierMethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_INSTANCE);
    EXPECT_STREQ(invokeInstanceStringSupplierMethodHandle->methodref()->get_name()->c_str(), "instanceStringSupplier");
    EXPECT_STREQ(invokeInstanceStringSupplierMethodHandle->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeInstanceStringSupplierMethodHandle->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  }

  {
    // Method Handle #6:
    //   type        : invoke-constructor
    //   target      : Ljava/lang/String; <init>
    //   target_type : (Ljava/lang/String;)V    
    auto invokeConstructorMethodHandle = idx->get_methodhandleidx(6);
    EXPECT_EQ(invokeConstructorMethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_CONSTRUCTOR);
    EXPECT_STREQ(invokeConstructorMethodHandle->methodref()->get_name()->c_str(), "<init>");
    EXPECT_STREQ(invokeConstructorMethodHandle->methodref()->get_class()->get_name()->c_str(), STRING_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeConstructorMethodHandle->methodref()->get_proto()), "()V");
  }

  {
    // Method Handle #7:
    //   type        : invoke-direct
    //   target      : Lcom/facebook/redextest/Dex038; privateInstanceStringSupplier
    //   target_type : (Lcom/facebook/redextest/Dex038;)Ljava/lang/String;  
    auto invokeDirectMethodHandle = idx->get_methodhandleidx(7);
    EXPECT_EQ(invokeDirectMethodHandle->type(), METHOD_HANDLE_TYPE_INVOKE_DIRECT);
    EXPECT_STREQ(invokeDirectMethodHandle->methodref()->get_name()->c_str(), "privateInstanceStringSupplier");
    EXPECT_STREQ(invokeDirectMethodHandle->methodref()->get_class()->get_name()->c_str(), DEX038_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeDirectMethodHandle->methodref()->get_proto()), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #0 // offset 2166
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : (Lcom/facebook/redextest/Dex038;)Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 5 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)    
    auto invokeInstanceStringSupplierCallsite = idx->get_callsiteidx(0);
    EXPECT_EQ(invokeInstanceStringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeInstanceStringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeInstanceStringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeInstanceStringSupplierCallsite->method_proto()->get_args()), DEX038_CLASS_NAME);
    EXPECT_EQ(invokeInstanceStringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeInstanceStringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeInstanceStringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(5)); 
    EXPECT_STREQ(SHOW(invokeInstanceStringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #1 // offset 2179
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : (Lcom/facebook/redextest/Dex038;)Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 7 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)
    auto invokeDirectStringSupplierCallsite = idx->get_callsiteidx(1);
    EXPECT_EQ(invokeDirectStringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeDirectStringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeDirectStringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeDirectStringSupplierCallsite->method_proto()->get_args()), DEX038_CLASS_NAME);
    EXPECT_EQ(invokeDirectStringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeDirectStringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeDirectStringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(7)); 
    EXPECT_STREQ(SHOW(invokeDirectStringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #2 // offset 2192
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 6 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)
    auto invokeConstructorStringSupplierCallsite = idx->get_callsiteidx(2);
    EXPECT_EQ(invokeConstructorStringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeConstructorStringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeConstructorStringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeConstructorStringSupplierCallsite->method_proto()->get_args()), "");
    EXPECT_EQ(invokeConstructorStringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeConstructorStringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeConstructorStringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(6)); 
    EXPECT_STREQ(SHOW(invokeConstructorStringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #3 // offset 2205
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 3 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)
    auto invokeStaticStringSupplierCallsite = idx->get_callsiteidx(3);
    EXPECT_EQ(invokeStaticStringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeStaticStringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeStaticStringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStaticStringSupplierCallsite->method_proto()->get_args()), "");
    EXPECT_EQ(invokeStaticStringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeStaticStringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeStaticStringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(3)); 
    EXPECT_STREQ(SHOW(invokeStaticStringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #4 // offset 2218
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 0 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)    
    auto invokeStatic$lambda$run$0StringSupplierCallsite = idx->get_callsiteidx(4);
    EXPECT_EQ(invokeStatic$lambda$run$0StringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeStatic$lambda$run$0StringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeStatic$lambda$run$0StringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$0StringSupplierCallsite->method_proto()->get_args()), "");
    EXPECT_EQ(invokeStatic$lambda$run$0StringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$0StringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeStatic$lambda$run$0StringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(0)); 
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$0StringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #5 // offset 2231
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 1 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)
    auto invokeStatic$lambda$run$1StringSupplierCallsite = idx->get_callsiteidx(5);
    EXPECT_EQ(invokeStatic$lambda$run$1StringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeStatic$lambda$run$1StringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeStatic$lambda$run$1StringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$1StringSupplierCallsite->method_proto()->get_args()), "");
    EXPECT_EQ(invokeStatic$lambda$run$1StringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$1StringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeStatic$lambda$run$1StringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(1)); 
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$1StringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  {
    // Call Site #6 // offset 2244
    //   link_argument[0] : 4 (MethodHandle)
    //   link_argument[1] : get (String)
    //   link_argument[2] : ()Ljava/util/function/Supplier; (MethodType)
    //   link_argument[3] : ()Ljava/lang/Object; (MethodType)
    //   link_argument[4] : 2 (MethodHandle)
    //   link_argument[5] : ()Ljava/lang/String; (MethodType)
    auto invokeStatic$lambda$run$2StringSupplierCallsite = idx->get_callsiteidx(6);
    EXPECT_EQ(invokeStatic$lambda$run$2StringSupplierCallsite->method_handle(), idx->get_methodhandleidx(4));
    EXPECT_STREQ(invokeStatic$lambda$run$2StringSupplierCallsite->method_name()->c_str(), "get");
    EXPECT_STREQ(invokeStatic$lambda$run$2StringSupplierCallsite->method_proto()->get_rtype()->get_name()->c_str(), SUPPLIER_CLASS_NAME);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$2StringSupplierCallsite->method_proto()->get_args()), "");
    EXPECT_EQ(invokeStatic$lambda$run$2StringSupplierCallsite->args().size(), 3);
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$2StringSupplierCallsite->args()[0]), VOID_RETURN_OBJECT_PROTO);
    EXPECT_EQ(((DexEncodedValueMethodHandle*)invokeStatic$lambda$run$2StringSupplierCallsite->args()[1])->methodhandle(), idx->get_methodhandleidx(2)); 
    EXPECT_STREQ(SHOW(invokeStatic$lambda$run$2StringSupplierCallsite->args()[2]), VOID_RETURN_STRING_PROTO);
  }

  // clang-format on
}

TEST(Dex038Test, ReadDex038) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  EXPECT_NE(nullptr, dexfile);

  testReadDex(dexfile);
}
