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

#include "Peephole.h"
#include "LocalDce.h"

/*

This test takes as input the Dex bytecode for the class generated
from the Java source file:
   <redex root>/test/integ/Propagation.java
which is specified in Buck tests via an enviornment variable in the
BUCK file. Before optimization, the code for the propagate method is:

dmethod: regs: 2, ins: 0, outs: 1
const-class Lcom/facebook/redextest/Propagation; v1
invoke-virtual java.lang.Class.getSimpleName()Ljava/lang/String; v1
move-result-object v0
return-object v0

After optimization with Peephole and LocalDCE the code should be:

dmethod: propagate
dmethod: regs: 2, ins: 0, outs: 1
const-string Propagation v0
return-object v0

This test checks to make sure the optimizations fired. It does
this by checking to make sure there are no OPCODE_INVOKE_VIRTUAL
instructions in the optimized method.

*/

TEST(PropagationTest1, localDCE1) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
  std::cout << "Loaded classes: " << classes.size() << std::endl ;

	TRACE(DCE, 2, "Code before:\n");
  for(const auto& cls : classes) {
	  TRACE(DCE, 2, "Class %s\n", SHOW(cls));
		for (const auto& dm : cls->get_dmethods()) {
		  TRACE(DCE, 2, "dmethod: %s\n",  dm->get_name()->c_str());
			if (strcmp(dm->get_name()->c_str(), "propagate") == 0) {
			  TRACE(DCE, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
			}
		}
	}

  std::vector<Pass*> passes = {
    new PeepholePass(),
    new LocalDcePass(),
  };

  std::vector<KeepRule> null_rules;
  PassManager manager(passes, null_rules);

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);
  manager.run_passes(dexen, dummy_cfg);

	TRACE(DCE, 2, "Code after:\n");
	for(const auto& cls : classes) {
	  TRACE(DCE, 2, "Class %s\n", SHOW(cls));
		for (const auto& dm : cls->get_dmethods()) {
		  TRACE(DCE, 2, "dmethod: %s\n",  dm->get_name()->c_str());
			if (strcmp(dm->get_name()->c_str(), "propagate") == 0) {
			  TRACE(DCE, 2, "dmethod: %s\n",  SHOW(dm->get_code()));
			  for (auto const instruction : dm->get_code()->get_instructions()) {
					// Make sure there is no invoke-virtual in the optimized method.
			    ASSERT_NE(instruction->opcode(), OPCODE_INVOKE_VIRTUAL);
          // Make sure there is no const-class in the optimized method.
          ASSERT_NE(instruction->opcode(), OPCODE_CONST_CLASS);
			  }
			}
		}
	}

}
