/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <json/json.h>

#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include "DexLoader.h"
#include "DexStore.h"
#include "InstructionLowering.h"
#include "tools/bytecode_debugger/InjectDebug.h"

#include <boost/filesystem.hpp>

/* Tests that the inject-debug program is able to run to completion and
 * output a new dex file. Tests are run on the output dex file to
 * ensure it is in the state that we want.
 */

class InjectDebugTest : public ::testing::Test {
 protected:
  InjectDebugTest() {
    reset_redex();
    m_test_dex_path = std::getenv("dex");
    m_tmp_dir = make_tmp_dir();

    // Always create primary dex file
    m_input_dex_paths.push_back(m_test_dex_path);
    m_output_dex_paths.push_back(m_tmp_dir + "/classes.dex");
  }

  void inject() {
    InjectDebug inject_debug(m_tmp_dir, m_input_dex_paths);
    inject_debug.run();
  }

  // Secondary dex files are in the form classesN.dex, N >= 2
  void create_secondary_dex(int index) {
    std::string name = "classes" + std::to_string(index);
    std::string input_dex_path = create_dir_with_dex(name);
    m_input_dex_paths.push_back(input_dex_path);
    m_output_dex_paths.push_back(m_tmp_dir + "/" + name + ".dex");
  }

  // Application Modules use DexMetadata files (e.g. ApplicationModule.json)
  // that contain a path to an input dex file.
  void create_metadata_dex(const std::string& module_name) {
    std::string module_dir = m_tmp_dir + "/" + module_name;
    std::string input_dex_path = create_dir_with_dex(module_name);

    std::string metadata_path = module_dir + "/" + module_name + ".json";
    Json::Value metadata;
    metadata["id"] = module_name;
    metadata["requires"][0] = "dex";
    metadata["files"][0] = input_dex_path;
    std::ofstream metadata_file(metadata_path);
    metadata_file << metadata;
    metadata_file.close();

    m_input_dex_paths.push_back(metadata_path);
    m_output_dex_paths.push_back(m_tmp_dir + "/" + module_name + "2.dex");
  }

  DexClasses load_classes(const std::string& path) {
    reset_redex();
    DexStore store("classes");
    store.add_classes(load_classes_from_dex(path.c_str(), /* balloon */ false));
    return store.get_dexen()[0];
  }

  // Helper to reduce duplicate code - runs a given function to fetch
  // information from classes and then checks equality
  // Compares the primary dex file output with the original input dex file
  void test_dex_equality_helper(
      const std::function<std::vector<std::string>(DexClasses)>& get_info) {
    std::vector<std::string> original_names, modified_names;
    original_names = get_info(load_classes(m_output_dex_paths[0]));
    modified_names = get_info(load_classes(m_test_dex_path));
    EXPECT_EQ(original_names.size(), modified_names.size());

    for (int i = 0; i < original_names.size(); ++i) {
      EXPECT_EQ(original_names[i], modified_names[i]);
    }
  }

  bool file_exists(const std::string& path) {
    std::ifstream file(path.c_str());
    return file.good();
  }

  std::string m_test_dex_path, m_tmp_dir;
  std::vector<std::string> m_input_dex_paths, m_output_dex_paths;

 private:
  void reset_redex() {
    delete g_redex;
    g_redex = new RedexContext(true);
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

  std::string create_dir_with_dex(const std::string& name) {
    std::string dex_path = m_tmp_dir + "/" + name + "/" + name + ".dex";
    boost::filesystem::create_directory(m_tmp_dir + "/" + name);
    copy_file(std::getenv("dex"), dex_path);
    return dex_path;
  }

  void copy_file(const std::string& src, const std::string& dest) {
    std::ifstream src_stream(src, std::ios::binary);
    std::ofstream dest_stream(dest, std::ios::binary);
    dest_stream << src_stream.rdbuf();
  }
};

// Check that general class data is unmodified by comparing class names
TEST_F(InjectDebugTest, TestClasses) {
  inject();
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
  inject();
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
  inject();
  test_dex_equality_helper(
      [](const DexClasses& classes) -> std::vector<std::string> {
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

// Check that multiple files can be processed at once, including metadata
TEST_F(InjectDebugTest, TestMultipleFiles) {
  create_secondary_dex(2);
  create_secondary_dex(3);
  create_metadata_dex("testmodule");
  inject();

  EXPECT_EQ(m_output_dex_paths.size(), 4);
  for (const std::string& out_path : m_output_dex_paths) {
    EXPECT_TRUE(file_exists(out_path));
  }
}
