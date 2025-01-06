/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/iostreams/device/mapped_file.hpp>

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

// This is temporary for refactoring purposes.
// Helper to get a DataUPtr that's backed by an mmap.
std::pair<DexLoader::DataUPtr, size_t> mmap_data(const char* dexfile) {
  auto mapped_file = std::make_unique<boost::iostreams::mapped_file>();

  mapped_file->open(dexfile, boost::iostreams::mapped_file::readonly);
  if (!mapped_file->is_open()) {
    fprintf(stderr, "error: cannot create memory-mapped file: %s\n", dexfile);
    exit(EXIT_FAILURE);
  }
  auto mapped_file_ptr = mapped_file.get();
  auto data = DexLoader::DataUPtr((const uint8_t*)mapped_file->const_data(),
                                  [mapped_file_ptr](auto*) {
                                    // Data is mapped, don't actually destroy
                                    // that, close the file and delete that.
                                    mapped_file_ptr->close();
                                    delete mapped_file_ptr;
                                  });
  // At this point we can release mapped_file.
  (void)mapped_file.release();

  return std::make_pair(std::move(data), mapped_file_ptr->size());
}

TEST(Dex039Test, ReadDex039) {
  // const-method-handle.dex is sourced from https://fburl.com/prikp912
  // ground truth dexdump is found at https://fburl.com/27ekisha

  const char* dexfile = std::getenv("dex");
  EXPECT_NE(nullptr, dexfile);

  g_redex = new RedexContext();

  // bare minium test to ensure the dex loads okay
  auto data = mmap_data(dexfile);
  auto dl = DexLoader::create(DexLocation::make_location("", dexfile),
                              std::move(data.first),
                              data.second,
                              39,
                              DexLoader::Parallel::kYes);
  const auto& classes = dl.get_classes();
  auto idx = dl.get_idx();

  // ensure that instructions can be shown
  std::ostringstream o;
  for (const auto& dex_class : classes) {
    o << show(dex_class);
    for (const auto& dex_method : dex_class->get_dmethods()) {
      o << show(dex_method);
      DexCode* code = dex_method->get_dex_code();
      for (const auto& dex_ins : code->get_instructions()) {
        o << show(dex_ins);
      }
    }
    for (const auto& dex_method : dex_class->get_vmethods()) {
      o << show(dex_method);
      DexCode* code = dex_method->get_dex_code();
      for (const auto& dex_ins : code->get_instructions()) {
        o << show(dex_ins);
      }
    }
  }
  auto parsed_code = o.str();

  EXPECT_NE(
      parsed_code.find("invoke-polymorphic "
                       "Ljava/lang/invoke/MethodHandle;.invokeExact:([Ljava/"
                       "lang/Object;)Ljava/lang/Object; v0, v5"),
      std::string::npos);
  EXPECT_NE(parsed_code.find("invoke-polymorphic "
                             "Ljava/lang/invoke/MethodHandle;.invoke:([Ljava/"
                             "lang/Object;)Ljava/lang/Object; v3, v2"),
            std::string::npos);
  EXPECT_NE(
      parsed_code.find("const-method-handle "
                       "Ljava/lang/Object;.getClass:()Ljava/lang/Class; v0"),
      std::string::npos);
  EXPECT_NE(
      parsed_code.find("const-method-type (CSIJFDLjava/lang/Object;)Z v0"),
      std::string::npos);
  EXPECT_EQ(idx->get_callsite_ids_size(), 0);
  EXPECT_EQ(idx->get_methodhandle_ids_size(), 1);
  EXPECT_EQ(idx->get_method_ids_size(), 23);
  EXPECT_EQ(idx->get_proto_ids_size(), 18);
  EXPECT_EQ(classes.size(), 2);
}
