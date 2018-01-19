/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

class CopyPropagationPass : public Pass {
 public:
  CopyPropagationPass() : Pass("CopyPropagationPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual void configure_pass(const PassConfig& pc) override {

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
    pc.get("eliminate_const_literals", false, m_config.eliminate_const_literals);

    pc.get("eliminate_const_strings", true, m_config.eliminate_const_strings);
    pc.get("eliminate_const_classes", true, m_config.eliminate_const_classes);
    pc.get("replace_with_representative",
           true,
           m_config.replace_with_representative);
    pc.get("wide_registers", false, m_config.wide_registers);
    pc.get("static_finals", false, m_config.static_finals);
    pc.get("debug", false, m_config.debug);
  }

  struct Config {
    bool eliminate_const_literals{false};
    bool eliminate_const_strings{true};
    bool eliminate_const_classes{true};
    bool replace_with_representative{true};
    bool wide_registers{false};
    bool static_finals{false};
    bool debug{false};

    // this is set by PassManager, not by PassConfig
    bool regalloc_has_run{false};
  } m_config;
};

namespace copy_propagation_impl {

struct Stats {
  size_t moves_eliminated{0};
  size_t replaced_sources{0};

  Stats() = default;
  Stats(size_t elim, size_t replaced)
      : moves_eliminated(elim), replaced_sources(replaced) {}

  Stats operator+(const Stats& other);
};

class CopyPropagation final {
 public:
  explicit CopyPropagation(const CopyPropagationPass::Config& config)
      : m_config(config) {}

  Stats run(Scope scope);

  Stats run(IRCode*);

 private:
  const CopyPropagationPass::Config& m_config;
};

} // namespace copy_propagation_impl
