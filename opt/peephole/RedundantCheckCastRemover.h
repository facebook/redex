// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <vector>
#include "DexClass.h"
#include "PassManager.h"

class RedundantCheckCastRemover {
 public:
  static const std::string& get_name() {
    static const std::string name("Remove_Redundant_CheckCast");
    return name;
  }

  explicit RedundantCheckCastRemover(PassManager& mgr,
                                     const std::vector<DexClass*>& scope);
  void run();

 private:
  static bool can_remove_check_cast(IRInstruction**, size_t);

  PassManager& m_mgr;
  const std::vector<DexClass*>& m_scope;
};
