// Copyright 2004-present Facebook. All Rights Reserved.

#include <vector>
#include "DexClass.h"
#include "PassManager.h"

class RedundantCheckCastRemover {
 public:
  explicit RedundantCheckCastRemover(PassManager& mgr,
                                     const std::vector<DexClass*>& scope);
  void run();

 private:
  static bool can_remove_check_cast(DexInstruction**, size_t);

  PassManager& m_mgr;
  const std::vector<DexClass*>& m_scope;
};
