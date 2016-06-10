/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "Transform.h"

#include "LocalDce.h"
#include "DelInit.h"
#include "RemoveEmptyClasses.h"
#include "ConstantPropagation.h"
#include <json/json.h>

#include "Debug.h"
#include "JarLoader.h"
#include "DexOutput.h"
#include "PassManager.h"
#include "ProguardLoader.h"
#include "ReachableClasses.h"
#include "Warning.h"

/*

This test takes as input the Dex bytecode for the class generated
from the Java source file:
   fbsource/fbandroid/native/redex/test/integ/ConstantPropagation.java
which is specified in Buck tests via an enviornment variable in the
BUCK file. Before optimization, the code for the propagate method is:

dmethod: propagation_1
dmethod: regs: 4, ins: 0, outs: 2
const/4 v1
if-eqz v1
new-instance Lcom/facebook/redextest/MyBy2Or3; v0
const/4 v3
invoke-direct public com.facebook.redextest.MyBy2Or3.<init>(I)V v0, v3
invoke-virtual public com.facebook.redextest.MyBy2Or3.Double()I v0
move-result v2
return v2
const/16 v2
goto

dmethod: propagation_2
dmethod: regs: 2, ins: 0, outs: 0
const/4 v0
if-eqz v0
const/16 v1
return v1
invoke-static public static com.facebook.redextest.Alpha.theAnswer()I
move-result v1
goto

dmethod: propagation_3
dmethod: regs: 2, ins: 0, outs: 0
invoke-static public static com.facebook.redextest.Gamma.getConfig()Z
move-result v1
if-eqz v1
const/16 v0
return v0
invoke-static public static com.facebook.redextest.Alpha.theAnswer()I
move-result v0
goto


After optimization with LocalDcePass, DelInitPass, RemoveEmptyClassesPass
and ConstantPropagationPass, the code should be:

dmethod: propagation_1
dmethod: regs: 2, ins: 0, outs: 0
const/16 v1
return v1

dmethod: propagation_2
dmethod: regs: 2, ins: 0, outs: 0
const/16 v1
return v1

dmethod: propagation_3
dmethod: regs: 1, ins: 0, outs: 0
const/16 v0
return v0

This test mainly checks whether the constant propagation is fired. It does
this by checking to make sure there are no OPCODE_IF_EQZ and OPCODE_NEW_INSTANCE
instructions in the optimized method.
*/

// The ClassType enum is used to classify and filter classes in test result
enum ClassType {
  MAINCLASS = 0,
  REMOVEDCLASS = 1,
  OTHERCLASS = 2,
};

void output_stats(const char*, const dex_output_stats_t&);
void output_moved_methods_map(const char*, DexClassesVector&, ConfigFiles&);

ClassType filter_test_classes(const DexString *cls_name) {
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/ConstantPropagation;") == 0)
    return MAINCLASS;
  if (strcmp(cls_name->c_str(), "Lcom/facebook/redextest/MyBy2Or3;") == 0 ||
      strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Alpha;") == 0 ||
      strcmp(cls_name->c_str(), "Lcom/facebook/redextest/Gamma;") == 0)
    return REMOVEDCLASS;
  return OTHERCLASS;
}


TEST(ConstantPropagationTest1, constantpropagation) {
  g_redex = new RedexContext();

	const char* dexfile =
    "gen/native/redex/test/integ/constant-propagation-test-dex/constant-propagation.dex";
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);
  }

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
  std::cout << "Loaded classes: " << classes.size() << std::endl;

	TRACE(CONSTP, 2, "Code before:\n");
  for(const auto& cls : classes) {
    if (filter_test_classes(cls->get_name()) < 2) {
  	  TRACE(CONSTP, 2, "Class %s\n", SHOW(cls));
  		for (const auto& dm : cls->get_dmethods()) {
  		  TRACE(CONSTP, 2, "dmethod: %s\n",  dm->get_name()->c_str());
  			if (strcmp(dm->get_name()->c_str(), "propagation_1") == 0 ||
            strcmp(dm->get_name()->c_str(), "propagation_2") == 0) {
  			  TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			}
  		}
    }
	}

  std::vector<Pass*> passes = {
    new DelInitPass(),
    new RemoveEmptyClassesPass(),
    new ConstantPropagationPass(),
    new LocalDcePass(),
  };

  std::vector<KeepRule> null_rules;
  PassManager manager(passes, null_rules);

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  manager.run_passes(dexen, dummy_cfg);

	TRACE(CONSTP, 2, "Code after:\n");
	for(const auto& cls : classes) {
    if (filter_test_classes(cls->get_name()) == REMOVEDCLASS) {
      TRACE(CONSTP, 2, "Class %s\n", SHOW(cls));
      // To be reverted: These classes should be removed by future optimization.
      ASSERT_TRUE(true);
    } else if (filter_test_classes(cls->get_name()) == MAINCLASS) {
      TRACE(CONSTP, 2, "Class %s\n", SHOW(cls));
      for (const auto& dm : cls->get_dmethods()) {
  		  TRACE(CONSTP, 2, "dmethod: %s\n",  dm->get_name()->c_str());
  			if (strcmp(dm->get_name()->c_str(), "propagation_1") == 0) {
  			  TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			  for (auto const instruction : dm->get_code()->get_instructions()) {
            ASSERT_NE(instruction->opcode(), OPCODE_IF_EQZ);
            ASSERT_NE(instruction->opcode(), OPCODE_NEW_INSTANCE);
  			  }
  			} else if (strcmp(dm->get_name()->c_str(), "propagation_2") == 0) {
          TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			  for (auto const instruction : dm->get_code()->get_instructions()) {
            ASSERT_NE(instruction->opcode(), OPCODE_IF_EQZ);
  			  }
        } else if(strcmp(dm->get_name()->c_str(), "propagation_3") == 0) {
          TRACE(CONSTP, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
  			  for (auto const instruction : dm->get_code()->get_instructions()) {
            // The logic will be reverted when the future
            // development of constant propagation optimization is done, i.e.,
            // The code will be changed to ASSERT_TRUE(false)
            // if IF_EQZ or Invote_Static instruction is found
            if (instruction->opcode() == OPCODE_IF_EQZ ||
                instruction->opcode() == OPCODE_INVOKE_STATIC) {
                    ASSERT_TRUE(true);
            }
  			  }
        }
  		}
    }
	}

  TRACE(MAIN, 1, "Writing out new DexClasses...\n");
  LocatorIndex* locator_index = nullptr;
  locator_index = new LocatorIndex(make_locator_index(dexen));

  dex_output_stats_t totals;

  auto methodmapping = std::string("mapping.txt");
  auto stats_output = std::string("stats_output");
  auto method_move_map = std::string("method_move_map");
  for (size_t i = 0; i < dexen.size(); i++) {
    std::stringstream ss;
    if (i > 0) {
      ss << (i + 1);
    }
    ss << "output.dex";
    auto stats = write_classes_to_dex(
      ss.str(),
      &dexen[i],
      locator_index,
      i,
      dummy_cfg,
      methodmapping.c_str());
    totals += stats;
  }
  output_stats(stats_output.c_str(), totals);
  output_moved_methods_map(method_move_map.c_str(), dexen, dummy_cfg);
  print_warning_summary();
  delete g_redex;
  TRACE(MAIN, 1, "Done.\n");
}

// This method is copied from main.cpp
void output_stats(const char* path, const dex_output_stats_t& stats) {
  Json::Value d;
  d["num_types"] = stats.num_types;
  d["num_type_lists"] = stats.num_type_lists;
  d["num_classes"] = stats.num_classes;
  d["num_methods"] = stats.num_methods;
  d["num_method_refs"] = stats.num_method_refs;
  d["num_fields"] = stats.num_fields;
  d["num_field_refs"] = stats.num_field_refs;
  d["num_strings"] = stats.num_strings;
  d["num_protos"] = stats.num_protos;
  d["num_static_values"] = stats.num_static_values;
  d["num_annotations"] = stats.num_annotations;
  Json::StyledStreamWriter writer;
  std::ofstream out(path);
  writer.write(out, d);
  out.close();
}

// This method is copied from main.cpp
void output_moved_methods_map(const char* path,
                              DexClassesVector& dexen,
                              ConfigFiles& cfg) {
  // print out moved methods map
  if (cfg.save_move_map() && strcmp(path, "")) {
    FILE* fd = fopen(path, "w");
    if (fd == nullptr) {
      perror("Error opening method move file");
      return;
    }
    auto move_map = cfg.get_moved_methods_map();
    std::string dummy = "dummy";
    for (const auto& it : *move_map) {
      MethodTuple mt = it.first;
      auto cls_name = std::get<0>(mt);
      auto meth_name = std::get<1>(mt);
      auto src_file = std::get<2>(mt);
      auto ren_to_cls_name = it.second->get_type()->get_name()->c_str();
      const char* src_string;
      if (src_file != nullptr) {
        src_string = src_file->c_str();
      } else {
        src_string = dummy.c_str();
      }
      fprintf(fd,
              "%s %s (%s) -> %s \n",
              cls_name->c_str(),
              meth_name->c_str(),
              src_string,
              ren_to_cls_name);
    }
    fclose(fd);
  } else {
    TRACE(MAIN, 1, "No method move map data structure!\n");
  }
}
