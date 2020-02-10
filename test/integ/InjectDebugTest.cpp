/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <functional>
#include <iostream>

#include "DexLoader.h"
#include "InstructionLowering.h"
#include "tools/bytecode-debugger/InjectDebug.h"

#include <boost/filesystem.hpp>

/* Tests that the inject-debug program is able to run to completion and
 * output a new dex file. Tests are run on the output dex file to
 * ensure it is in the state that we want.
 */

class InjectDebugTest : public ::testing::Test {
 protected:
  InjectDebugTest() {}

  void SetUp() override {
    reset_redex();
    m_dex_path = std::getenv("dex");
    m_tmp_dir = make_tmp_dir();
    m_modified_dex_path = m_tmp_dir + "/classes.dex";

    std::cout << "(SetUp) Using dex file at " << m_dex_path << std::endl;
    std::cout << "(SetUp) Using temp directory at " << m_tmp_dir << std::endl;

    // Create new dex file with injected debug information
    InjectDebug inject_debug(m_tmp_dir, {m_dex_path});
    inject_debug.run();
  }

  std::string make_tmp_dir() {
    boost::filesystem::path dex_dir = boost::filesystem::temp_directory_path();
    dex_dir +=
        boost::filesystem::unique_path("/redex_inject_debug_test%%%%%%%%");
    boost::filesystem::create_directories(dex_dir);
    boost::filesystem::path meta_dir(dex_dir.string() + "/meta");
    boost::filesystem::create_directories(meta_dir);
    return dex_dir.string();
  }

  void reset_redex() {
    delete g_redex;
    g_redex = new RedexContext(true);
  }

  DexClasses load_classes(const std::string& path) {
    reset_redex();
    DexStore store("classes");
    store.add_classes(load_classes_from_dex(path.c_str(), /* balloon */ false));
    DexClasses classes = store.get_dexen()[0];
    return classes;
  }

  // Helper to reduce duplicate code - runs a given function to fetch
  // information from classes and then checks equality
  void test_dex_equality_helper(
      const std::function<std::vector<std::string>(DexClasses)>& get_info) {
    std::vector<std::string> original_names, modified_names;
    original_names = get_info(load_classes(m_dex_path));
    modified_names = get_info(load_classes(m_modified_dex_path));

    EXPECT_EQ(original_names.size(), modified_names.size());

    for (int i = 0; i < original_names.size(); ++i) {
      EXPECT_EQ(original_names[i], modified_names[i]);
    }
  }

  std::string m_dex_path, m_tmp_dir, m_modified_dex_path;
};

// Check that general class data is unmodified by comparing class names
TEST_F(InjectDebugTest, TestClasses) {
  test_dex_equality_helper(
      [](const DexClasses& classes) -> std::vector<std::string> {
        std::vector<std::string> class_names;
        for (DexClass* dex_class : classes) {
          class_names.push_back(dex_class->str());
        }
        return class_names;
      });
}

// Check that general method data is unmodified by comparing method names
TEST_F(InjectDebugTest, TestMethods) {
  test_dex_equality_helper(
      [](const DexClasses& classes) -> std::vector<std::string> {
        std::vector<std::string> method_names;
        for (DexClass* dex_class : classes) {
          for (DexMethod* dex_method : dex_class->get_dmethods())
            method_names.push_back(dex_method->str());
          for (DexMethod* dex_method : dex_class->get_vmethods())
            method_names.push_back(dex_method->str());
        }
        return method_names;
      });
}

// Check that general code data is unmodified by comparing instructions
TEST_F(InjectDebugTest, TestCodeItems) {
  test_dex_equality_helper([](DexClasses classes) -> std::vector<std::string> {
    std::vector<std::string> instructions;
    for (int i = 0; i < classes.size(); ++i) {
      for (DexMethod* dex_method : classes[i]->get_dmethods()) {
        for (DexInstruction* dex_instr :
             dex_method->get_dex_code()->get_instructions()) {
          instructions.push_back(show(dex_instr));
        }
      }
    }
    return instructions;
  });
}
