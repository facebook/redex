/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "AbstractDomain.h"
#include "ConstantAbstractDomain.h"
#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "HashedAbstractEnvironment.h"
#include "MonotonicFixpointIterator.h"

/*
 * Pattern we are trying to match:
 *  SGET_OBJECT <LookupTable>;.table
 *  MOVE_RESULT_PSEUDO <v_field>
 *  ...
 *  INVOKE_VIRTUAL <v_enum> <Enum>;.ordinal:()
 *  MOVE_RESULT <v_ordinal>
 *  ...
 *  AGET <v_field>, <v_ordinal>
 *  MOVE_RESULT_PSEUDO <v_dest>
 *  ...
 *  *_SWITCH <v_dest>
 *
 * But we want to find cases that have interleaved unrelated instructions or
 * block boundaries between them, so we use the sparta analysis framework.
 *
 * We track information about which instructions wrote to a given register in
 * the `info` struct. If we reach a switch statement with all the fields filled,
 * then we've matched the pattern.
 *
 * The iterator is used in two phases. First, `iterator.run()` does the analysis
 * and `collect()` finds all the sequences that match the pattern
 */

namespace optimize_enums {

struct Info {
  DexField* array_field{nullptr};
  boost::optional<cfg::InstructionIterator> invoke;
  boost::optional<cfg::InstructionIterator> aget;
  boost::optional<cfg::InstructionIterator> switch_ordinal;

  bool operator==(const Info& other) const {
    return array_field == other.array_field && invoke == other.invoke &&
           aget == other.aget && switch_ordinal == other.switch_ordinal;
  }

  std::string str() const {
    std::ostringstream o;
    o << "Info{\n";
    if (array_field) {
      o << "  " << show(array_field) << "\n";
    }
    if (invoke) {
      o << "  " << show(**invoke) << "\n";
    }
    if (aget) {
      o << "  " << show(**aget) << "\n";
    }
    if (switch_ordinal) {
      o << "  " << show(**switch_ordinal) << "\n";
    }
    o << "}";
    return o.str();
  }
};

using Domain = sparta::ConstantAbstractDomain<Info>;
using Environment = sparta::HashedAbstractEnvironment<reg_t, Domain>;

class Iterator final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               Environment> {
 public:
  explicit Iterator(cfg::ControlFlowGraph* cfg)
      : MonotonicFixpointIterator(*cfg), m_cfg(cfg) {}
  virtual void analyze_node(cfg::Block* const& block,
                            Environment* env) const override;
  virtual Environment analyze_edge(
      cfg::Edge* const& edge,
      const Environment& exit_state_at_source) const override;

  std::vector<Info> collect() const;

 private:
  void analyze_insn(cfg::InstructionIterator it, Environment* env) const;
  cfg::ControlFlowGraph* m_cfg;
};

} // namespace optimize_enums
