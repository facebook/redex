/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexTest.h"
#include "TypeInference.h"

using namespace testing;

class TypeInferenceTest : public RedexTest {};

TEST_F(TypeInferenceTest, const0) {
  auto method = assembler::method_from_string(R"(
    (method (private) "LFoo;.bar:()V"
     (
      (load-param-object v1) ; 'this' argument
      (iget-object v1 "LFoo;.a:LBar;")
      (move-result-pseudo-object v0)
      (const v0 0)
      (invoke-interface (v0) "LBaz;.heh:()V")  ; v0 should not be LBar type
      (return-void)
     )
    )
  )");
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg);
  inference.run(method);
  auto& envs = inference.get_type_environments();
  for (auto& mie : InstructionIterable(*code)) {
    auto insn = mie.insn;
    if (opcode::is_an_invoke(insn->opcode())) {
      auto& env = envs.at(insn);
      auto dex_type = env.get_dex_type(insn->src(0));
      if (dex_type && *dex_type != type::java_lang_Object()) {
        EXPECT_TRUE(DexType::get_type("LBar;") != *dex_type);
      }
    }
  }
}
