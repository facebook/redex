/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class AccessMarkingPass : public Pass {
 public:
  AccessMarkingPass() : Pass("AccessMarkingPass") {}

  std::string get_config_doc() override {
    return "This pass will mark class, methods, and fields final, when able to "
           "do so. This is generally advantageous for performance. It will "
           "also mark methods private when able to do so, for the same reason.";
  }

  void bind_config() override {
    bind("finalize_classes", true, m_finalize_classes,
         "Mark every non-abstract class as final.");
    bind("finalize_methods", true, m_finalize_methods,
         "Mark every non-abstract method as final.");
    bind("finalize_fields", true, m_finalize_fields,
         "Mark every non-final, non-volatile field as final.");
    bind("privatize_methods", true, m_privatize_methods,
         "Mark every eligible method as private.");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_finalize_classes{true};
  bool m_finalize_methods{true};
  bool m_finalize_fields{true};
  bool m_privatize_methods{true};
};
