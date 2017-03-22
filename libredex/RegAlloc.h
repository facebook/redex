/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/dynamic_bitset.hpp>
#include <memory>

#include "DexClass.h"

struct Block;

using RegSet = boost::dynamic_bitset<>;
using LivenessMap = std::unordered_map<IRInstruction*, Liveness>;

class Liveness {
  RegSet m_reg_set;
 public:
  Liveness(int nregs): m_reg_set(nregs) {}
  Liveness(const RegSet&& reg_set): m_reg_set(std::move(reg_set)) {}

  const RegSet& bits() { return m_reg_set; }

  void meet(const Liveness&);
  bool operator==(const Liveness&) const;
  bool operator!=(const Liveness& that) const {
    return !(*this == that);
  };
  void enlarge(uint16_t ins_size, uint16_t newregs);

  static void trans(const IRInstruction*, Liveness*);
  static std::unique_ptr<LivenessMap> analyze(ControlFlowGraph&,
                                              uint16_t nregs);

  friend std::string show(const Liveness&);
};

void allocate_registers(DexMethod*);
