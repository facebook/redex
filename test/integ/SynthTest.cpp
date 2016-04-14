/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

#include <folly/json.h>
#include <folly/dynamic.h>

#include "DexClass.h"
#include "DexOpcode.h"
#include "DexLoader.h"
#include "PassManager.h"
#include "RedexContext.h"

#include "ReBindRefs.h"
#include "Synth.h"

// NOTE: this is not really a unit test.

/*
To understand this test one needs to also look at the file
Java source file Alpha.java in the same directory.
This Java source file compiled and a corresponding Dex file
is created which is an input to this test. This test runs
the prelimninary ReBindRefsPass pass and then the SynthPass
which is the subject of this test.

The Alpha class has an inner class Beta and there is an access
inside Beta to a static field of Alpha which induces a synthetic
wrapper. This test makes sure this wrapper method is removed.

The Gamma class has an inner class Delta which has a non-concrete
access to a field that is declared elesewhere. This test checks
to make sure we do not optimize such synthetic getters.

*/

TEST(SynthTest1, synthetic) {
  g_redex = new RedexContext();

  const char* dexfile = "synth-test-class.dex";
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);
  }

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
  std::cout << "Loaded classes: " << classes.size() << std::endl ;

  std::vector<Pass*> passes = {
    new ReBindRefsPass(),
    new SynthPass(),
  };

  std::vector<KeepRule> null_rules;
  PassManager manager(passes, null_rules);
  manager.run_passes(dexen);

  // Make sure synthetic method is removed from class Alpha.
  for (const auto& cls : classes) {
    const auto class_name = cls->get_type()->get_name()->c_str();
    // Make sure the synthetic method has been removed.
    if (strcmp(class_name, "Lcom/facebook/redextest/Alpha;") == 0) {
      for (const auto& method : cls->get_dmethods()) {
        ASSERT_STRNE("access$000",  method->get_name()->c_str());
      }
    }

    // Make sure there are no references to the synthetic method.
    if (strcmp(class_name, "Lcom/facebook/redextest/Alpha$Beta;") == 0) {
      for (const auto& method : cls->get_dmethods()) {
        for (const auto& method : cls->get_vmethods()) {
          const auto& code = method->get_code();
          const auto& opcodes = code->get_instructions();
          for (auto& inst : opcodes) {
            std::cout << SHOW(inst) << std::endl;
            if (is_invoke(inst->opcode())) {
              auto invoke = static_cast<DexOpcodeMethod*>(inst);
              const auto clazz =
                  invoke->get_method()->get_class()->get_name()->c_str();
              const auto n =  invoke->get_method()->get_name()->c_str();
              auto invocation = std::string(clazz) + "." + std::string(n);
              ASSERT_STRNE("Lcom/facebook/redextest/Alpha;.access$000",
              invocation.c_str());
            }
          }
        }
      }
    }

    // Make sure we don't apply the optimization in cases where the field
    // is not concrete.
    if (strcmp(class_name, "Lcom/facebook/redextest/Gamma;") == 0) {
      bool gamma_synth_found = false;
      for (const auto& method : cls->get_dmethods()) {
        if (strcmp(method->get_name()->c_str(), "access$000") == 0) {
          gamma_synth_found = true;
          break;
        }
      }
      ASSERT_TRUE(gamma_synth_found);
    }
  }

  delete g_redex;
}
