/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class CopyPropagationPass : public Pass {
 public:
  CopyPropagationPass() : Pass("CopyPropagationPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void configure_pass(const JsonWrapper& jw) override {

    // This option can only be safely enabled in verify-none. `run_pass` will
    // override this value to false if we aren't in verify-none. Here's why:
    //
    // const v0, 0
    // sput v0, someFloat   # uses v0 as a float
    // const v0, 0          # This could be eliminated (in verify-none)
    // sput v0, someInt     # uses v0 as an int
    //
    // The android verifier insists on having the second const load because
    // using v0 as a float gives it type float. But, in reality the bits in the
    // register are the same, so in verify none mode, we can eliminate the
    // second const load
    //
    // TODO: detect the type of constant for each alias group
    jw.get(
        "eliminate_const_literals", false, m_config.eliminate_const_literals);

    jw.get("eliminate_const_strings", true, m_config.eliminate_const_strings);
    jw.get("eliminate_const_classes", true, m_config.eliminate_const_classes);
    jw.get("replace_with_representative",
           true,
           m_config.replace_with_representative);
    jw.get("wide_registers", false, m_config.wide_registers);
    jw.get("static_finals", false, m_config.static_finals);
    jw.get("debug", false, m_config.debug);
    jw.get("max_estimated_registers", 3000, m_config.max_estimated_registers);
  }

  struct Config {
    bool eliminate_const_literals{false};
    bool eliminate_const_strings{true};
    bool eliminate_const_classes{true};
    bool replace_with_representative{true};
    bool wide_registers{false};
    bool static_finals{false};
    bool debug{false};

    // this is set by PassManager, not by JsonWrapper
    bool regalloc_has_run{false};
    size_t max_estimated_registers{3000};
  } m_config;
};

namespace copy_propagation_impl {

struct Stats {
  size_t moves_eliminated{0};
  size_t replaced_sources{0};
  size_t skipped_due_to_too_many_registers{0};

  Stats() = default;
  Stats(size_t elim, size_t replaced, size_t skipped)
      : moves_eliminated(elim),
        replaced_sources(replaced),
        skipped_due_to_too_many_registers(skipped) {}

  Stats operator+(const Stats& other);
};

class CopyPropagation final {
 public:
  explicit CopyPropagation(const CopyPropagationPass::Config& config)
      : m_config(config) {}

  Stats run(Scope scope);

  Stats run(IRCode*, DexMethod* = nullptr);

 private:
  const CopyPropagationPass::Config& m_config;
};

} // namespace copy_propagation_impl
