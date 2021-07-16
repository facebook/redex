/*
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
#include "DirectProductAbstractDomain.h"
#include "HashedAbstractEnvironment.h"
#include "MonotonicFixpointIterator.h"

/*
 * Pattern we are trying to match:
 *  // The null checking may or may not exist.
 *  IF_NEZ <v_enum> :NON-NULL-LABEL
 *  CONST <v_dest> -1 // or any negative value
 *  GOTO :SWITCH-LABEL
 *
 *  :NON-NULL-LABEL
 *  SGET_OBJECT <LookupTable>;.table
 *  MOVE_RESULT_PSEUDO <v_field>
 *  ...
 *  INVOKE_VIRTUAL <v_enum> <Enum>;.ordinal:()
 *  MOVE_RESULT <v_ordinal>
 *  ...
 *  AGET <v_field>, <v_ordinal>
 *  MOVE_RESULT_PSEUDO <v_dest>
 *  ...
 *
 *  :SWITCH-LABEL
 *  *_SWITCH <v_dest>            ; or IF_EQZ <v_dest> <v_some_constant>
 *
 *
 * But we want to find cases that have interleaved unrelated instructions or
 * block boundaries between them, so we use the sparta analysis framework.
 * Also, we need to handle switches that have been turned into if-else chains by
 * D8, so we actually look for enums in branch instructions, not just switches.
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
  boost::optional<cfg::InstructionIterator> branch;
  boost::optional<reg_t> reg;

  bool operator==(const Info& other) const {
    return array_field == other.array_field && invoke == other.invoke &&
           aget == other.aget && branch == other.branch;
  }

  friend std::ostream& operator<<(std::ostream& out, const Info& info) {
    out << "Info{\n";
    if (info.array_field) {
      out << "  " << show(info.array_field) << "\n";
    }
    if (info.invoke) {
      out << "  " << show(**info.invoke) << "\n";
    }
    if (info.aget) {
      out << "  " << show(**info.aget) << "\n";
    }
    if (info.branch) {
      out << "bbranch:  " << show(**info.branch) << "\n";
    }
    if (info.reg) {
      out << "  " << *info.reg << "\n";
    }
    out << "}";
    return out;
  }
};

// The key can be a value from lookup table or a negative constant.
using EnumSwitchKey = std::pair<Info, boost::optional<int32_t>>;

using InfoDomain = sparta::ConstantAbstractDomain<Info>;
using ConstantDomain = sparta::ConstantAbstractDomain<int32_t>;

class Domain : public sparta::DirectProductAbstractDomain<Domain,
                                                          InfoDomain,
                                                          ConstantDomain> {
 public:
  using DirectProductAbstractDomain::DirectProductAbstractDomain;
  Domain() = default;
  explicit Domain(int64_t v)
      : Domain(std::make_tuple(InfoDomain::bottom(), ConstantDomain(v))) {}
  explicit Domain(const Info& info)
      : Domain(std::make_tuple(InfoDomain(info), ConstantDomain::bottom())) {}
  Domain(Info& info, const ConstantDomain& v)
      : Domain(std::make_tuple(InfoDomain(info), v)) {}

  static void reduce_product(
      std::tuple<InfoDomain, ConstantDomain>& /* domains */) {}

  boost::optional<InfoDomain::ConstantType> get_info() const {
    return get<0>().get_constant();
  }

  boost::optional<ConstantDomain::ConstantType> get_constant() const {
    return get<1>().get_constant();
  }

  Domain combine_with_reg(reg_t reg) const {
    auto info = get_info();
    info->reg = reg;
    return Domain(*info, get<1>());
  }

  Domain combine_with_branch(cfg::InstructionIterator& branch) const {
    auto info = get_info();
    info->branch = branch;
    return Domain(*info, get<1>());
  }
};

using Environment = sparta::HashedAbstractEnvironment<reg_t, Domain>;

class Iterator final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               Environment> {
 public:
  explicit Iterator(cfg::ControlFlowGraph* cfg)
      : MonotonicFixpointIterator(*cfg), m_cfg(cfg) {}
  void analyze_node(cfg::Block* const& block, Environment* env) const override;
  Environment analyze_edge(
      cfg::Edge* const& edge,
      const Environment& exit_state_at_source) const override;

  std::vector<EnumSwitchKey> collect() const;

 private:
  void analyze_insn(const cfg::InstructionIterator& it, Environment* env) const;
  cfg::ControlFlowGraph* m_cfg;
};

} // namespace optimize_enums
