/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <iostream>

#include "ControlFlow.h"
#include "FixpointIterators.h"
#include "HashedSetAbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"
#include "SignDomain.h"

/**
 *This class represents constant values living in the following lattice:
 *                           T
 *
 *                  /        |         \
 *
 * [Narrow (32 bit) and wide (64 bit) width integral constants]
 *
 *                  \        |        /
 *
 *                          _|_
 *
 * NOTE: ConstantValue is unaware of type (integral vs floating point) it just
 *       knows about width (one 32 bit register or a register pair representing
 *       64 bit values.
 */
class ConstantValue final : public AbstractValue<ConstantValue> {
 public:
  friend class ConstantDomain;

  enum ConstantType { NARROW, WIDE, INVALID };

  ConstantValue() : m_value(-1), m_type(ConstantType::INVALID) {}

  void clear() override {}

  Kind kind() const override { return Kind::Value; }

  bool equals(const ConstantValue& other) const override {
    return m_value == other.m_value && m_type == other.m_type;
  }

  bool leq(const ConstantValue& other) const override { return equals(other); }

  Kind join_with(const ConstantValue& other) override {
    if (!equals(other)) {
      return Kind::Top;
    } else {
      return Kind::Value;
    }
  }

  Kind meet_with(const ConstantValue& other) override {
    if (!equals(other)) {
      return Kind::Bottom;
    } else {
      return Kind::Value;
    }
  }

  Kind widen_with(const ConstantValue& other) override {
    return join_with(other);
  }

  Kind narrow_with(const ConstantValue& other) override {
    return meet_with(other);
  }

  int64_t constant() const { return m_value; }
  ConstantType type() const { return m_type; }

  ConstantValue(int64_t value, ConstantType type)
      : m_value(value), m_type(type) {}

 private:
  int64_t m_value;
  ConstantType m_type;
};

// Add operator overloading for == so that 3rd-party code that depends on it
// (like gtest's EXPECT_EQ) will work
inline bool operator==(const ConstantValue& v1, const ConstantValue& v2) {
  return v1.equals(v2);
}

inline bool operator!=(const ConstantValue& v1, const ConstantValue& v2) {
  return !(v1 == v2);
}

std::ostream& operator<<(std::ostream& o, const ConstantValue& cv);

class ConstantDomain final
    : public AbstractDomainScaffolding<ConstantValue, ConstantDomain> {
 public:
  ConstantDomain(AbstractValueKind kind = AbstractValueKind::Top)
      : AbstractDomainScaffolding<ConstantValue, ConstantDomain>(kind) {}

  ConstantValue value() const { return *get_value(); }

  static ConstantDomain bottom() {
    return ConstantDomain(AbstractValueKind::Bottom);
  }

  static ConstantDomain top() { return ConstantDomain(AbstractValueKind::Top); }

  static ConstantDomain value(int64_t v, ConstantValue::ConstantType type) {
    assert(type != ConstantValue::ConstantType::INVALID);
    ConstantDomain result;
    result.set_to_value(ConstantValue(v, type));
    return result;
  }

  std::string str() const;

  friend class ConstPropEnvUtil;
};

class SignedConstantDomain
    : public ReducedProductAbstractDomain<SignedConstantDomain,
                                          sign_domain::Domain,
                                          ConstantDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  SignedConstantDomain() = default;

  explicit SignedConstantDomain(int64_t v, ConstantValue::ConstantType type)
      : SignedConstantDomain(std::make_tuple(sign_domain::Domain::top(),
                                             ConstantDomain::value(v, type))) {}

  explicit SignedConstantDomain(sign_domain::Interval interval)
      : SignedConstantDomain(std::make_tuple(sign_domain::Domain(interval),
                                             ConstantDomain::top())) {}

  static void reduce_product(
      std::tuple<sign_domain::Domain, ConstantDomain>& domains) {
    auto& sdom = std::get<0>(domains);
    auto& cdom = std::get<1>(domains);
    if (!cdom.is_value()) {
      return;
    }
    auto cst = cdom.value().constant();
    if (!sign_domain::contains(sdom.element(), cst)) {
      sdom.set_to_bottom();
      return;
    }
    sdom.meet_with(sign_domain::from_int(cst));
  }

  sign_domain::Domain interval_domain() const { return get<0>(); }

  sign_domain::Interval interval() const { return interval_domain().element(); }

  ConstantDomain constant_domain() const { return get<1>(); }

  static SignedConstantDomain top() {
    SignedConstantDomain scd;
    scd.set_to_top();
    return scd;
  }

  static SignedConstantDomain bottom() {
    SignedConstantDomain scd;
    scd.set_to_bottom();
    return scd;
  }

  /* Return the largest element within the interval. */
  int64_t max_element() const;

  /* Return the smallest element within the interval. */
  int64_t min_element() const;
};

inline bool operator==(const SignedConstantDomain& x,
                       const SignedConstantDomain& y) {
  return x.equals(y);
}

inline bool operator!=(const SignedConstantDomain& x,
                       const SignedConstantDomain& y) {
  return !(x == y);
}

std::ostream& operator<<(std::ostream& o, const ConstantDomain& cd);

using ConstPropEnvironment =
    PatriciaTreeMapAbstractEnvironment<uint16_t, SignedConstantDomain>;

class ConstPropEnvUtil {
 public:
  static ConstPropEnvironment& set_narrow(ConstPropEnvironment& env,
                                          uint16_t reg,
                                          int32_t value);
  static ConstPropEnvironment& set_wide(ConstPropEnvironment& env,
                                        uint16_t reg,
                                        int64_t value);
  static ConstPropEnvironment& set_top(ConstPropEnvironment& env,
                                       uint16_t reg,
                                       bool is_wide = false);
  static bool is_narrow_constant(const ConstPropEnvironment& env, int16_t reg);
  static bool is_wide_constant(const ConstPropEnvironment& env,
                               int16_t reg);
  static int32_t get_narrow(const ConstPropEnvironment& env, int16_t reg);
  static int64_t get_wide(const ConstPropEnvironment& env, int16_t reg);
};

/**
 * Implements intraprocedural constant-propagation dataflow by using
 * the abstract interpretation framework.
 *
 * This code works in two phases:
 * Phase 1:  First gather all the facts about constant and model them inside the
 *           constants lattice (described above).  Run the fixpoint analysis and
 *           propagate all facts throughout the CFG.  In code these are all the
 *           analyze_*() functions.
 *
 * Phase 2:  Once we reached a fix point, then replay the analysis but this time
 *           use the previously gathered facts about constant and use them to
 *           replace instructions.  In code; these are all the simplify_*
 *           functions.
 */
template <typename GraphInterface,
          typename InstructionType,
          typename BlockIterable,
          typename InstructionIterable>
class ConstantPropFixpointAnalysis
    : public MonotonicFixpointIterator<GraphInterface, ConstPropEnvironment> {
 public:
  using Graph = typename GraphInterface::Graph;
  using BlockType = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;

  ConstantPropFixpointAnalysis(const Graph& graph,
                               BlockIterable const& cfg_iterable)
      : MonotonicFixpointIterator<GraphInterface, ConstPropEnvironment>(graph),
        m_cfg_iterable(cfg_iterable) {}

  void simplify() const {
    for (const auto& block : m_cfg_iterable) {
      auto state = this->get_entry_state_at(block);
      for (auto& insn : InstructionIterable(block)) {
        analyze_instruction(insn, &state);
        simplify_instruction(block, insn, state);
      }
    }
  }

  ConstPropEnvironment analyze_edge(
      const EdgeId&,
      const ConstPropEnvironment& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_node(BlockType const& block,
                    ConstPropEnvironment* state_at_entry) const override {
    TRACE(CONSTP, 5, "Analyzing block: %d\n", block->id());
    for (auto& insn : InstructionIterable(block)) {
      analyze_instruction(insn, state_at_entry);
    }
  }

  ConstPropEnvironment get_constants_at_entry(BlockType const& node) const {
    return this->get_entry_state_at(node);
  }

  ConstPropEnvironment get_constants_at_exit(BlockType const& node) const {
    return this->get_exit_state_at(node);
  }

  virtual void simplify_instruction(
      const BlockType& block,
      InstructionType& insn,
      const ConstPropEnvironment& current_state) const = 0;

  virtual void analyze_instruction(
      const InstructionType& insn,
      ConstPropEnvironment* current_state) const = 0;

 private:
  BlockIterable m_cfg_iterable;
};
