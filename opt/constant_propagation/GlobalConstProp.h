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
#include <queue>

#include "ControlFlow.h"
#include "FixpointIterators.h"
#include "HashedAbstractEnvironment.h"
#include "HashedSetAbstractDomain.h"

class ReachableBlocksAdapter {
 public:
  explicit ReachableBlocksAdapter(const ControlFlowGraph&);
  std::vector<Block*>& succ(const Block* b) const;
  std::vector<Block*>& pred(const Block* b) const;

 private:
  const ControlFlowGraph& cfg;
  std::vector<Block*> m_succ;
  std::vector<Block*> m_pred;
};

class ConstantValue final : public AbstractValue<ConstantValue> {
 public:
  friend class ConstantDomain;

  enum ConstantType { NARROW, WIDE_A, WIDE_B, INVALID };

  ConstantValue() : m_value(-1), m_type(ConstantType::INVALID) {}

  void clear() override{};
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

  int32_t constant() const { return m_value; }
  ConstantType type() const { return m_type; }

  ConstantValue(int32_t value, ConstantType type)
      : m_value(value), m_type(type) {}

 private:
  int32_t m_value;
  ConstantType m_type;
};

std::ostream& operator<<(std::ostream& o, const ConstantValue& cv) {
  o << "ConstantValue[ Type:";
  switch (cv.type()) {
  case ConstantValue::ConstantType::NARROW: {
    o << "NARROW";
    break;
  }
  case ConstantValue::ConstantType::WIDE_A: {
    o << "WIDE_A";
    break;
  }
  case ConstantValue::ConstantType::WIDE_B: {
    o << "WIDE_B";
    break;
  }
  case ConstantValue::ConstantType::INVALID: {
    o << "<INVALID>";
    break;
  }
  }
  o << ", Value: " << cv.constant() << "]";
  return o;
}

class ConstantDomain final
    : public AbstractDomainScaffolding<ConstantValue, ConstantDomain> {
 private:
  static ConstantDomain value(int32_t v, ConstantValue::ConstantType type) {
    assert(type != ConstantValue::ConstantType::INVALID);
    ConstantDomain result;
    result.set_to_value(ConstantValue(v, type));
    return result;
  }

 public:
  ConstantDomain(AbstractValueKind kind = AbstractValueKind::Top)
      : AbstractDomainScaffolding<ConstantValue, ConstantDomain>(kind) {}

  ConstantValue value() const { return *get_value(); }

  static ConstantDomain bottom() {
    return ConstantDomain(AbstractValueKind::Bottom);
  }

  static ConstantDomain top() { return ConstantDomain(AbstractValueKind::Top); }

  friend class ConstPropEnvUtil;
};

std::ostream& operator<<(std::ostream& o, const ConstantDomain& cd) {
  if (cd.is_bottom()) {
    o << "_|_";
  } else if (cd.is_top()) {
    o << "T";
  } else {
    o << cd.value();
  }
  return o;
}

using ConstPropEnvironment =
    HashedAbstractEnvironment<uint16_t, ConstantDomain>;

class ConstPropEnvUtil {
 public:
  static ConstPropEnvironment& set_narrow(ConstPropEnvironment& env,
                                          uint16_t reg,
                                          int32_t value) {
    env.set(reg,
            ConstantDomain::value(value, ConstantValue::ConstantType::NARROW));
    return env;
  }

  static ConstPropEnvironment& set_wide(ConstPropEnvironment& env,
                                        uint16_t first_reg,
                                        int64_t value) {
    int32_t first_half = (int32_t)((value >> 32) & 0xFFFFFFFFL);
    int32_t second_half = (int32_t)(value & 0xFFFFFFFFL);
    env.set(
        first_reg,
        ConstantDomain::value(first_half, ConstantValue::ConstantType::WIDE_A));
    env.set(first_reg + 1,
            ConstantDomain::value(second_half,
                                  ConstantValue::ConstantType::WIDE_B));
    return env;
  }

  static ConstPropEnvironment& set_top(ConstPropEnvironment& env,
                                       uint16_t first_reg,
                                       bool is_wide = false) {
    env.set(first_reg, ConstantDomain::top());
    if (is_wide) {
      env.set(first_reg + 1, ConstantDomain::top());
    }
    return env;
  }

  static bool is_narrow_constant(const ConstPropEnvironment& env, int16_t reg) {
    const auto& domain = env.get(reg);
    return domain.is_value() &&
           domain.value().type() == ConstantValue::ConstantType::NARROW;
  }

  static bool is_wide_constant(const ConstPropEnvironment& env,
                               int16_t first_reg) {
    const auto& domain1 = env.get(first_reg);
    const auto& domain2 = env.get(first_reg + 1);
    return domain1.is_value() && domain2.is_value() &&
           domain1.value().type() == ConstantValue::ConstantType::WIDE_A &&
           domain2.value().type() == ConstantValue::ConstantType::WIDE_B;
  }

  static int32_t get_narrow(const ConstPropEnvironment& env, int16_t reg) {
    assert(is_narrow_constant(env, reg));
    return env.get(reg).value().constant();
  }

  static int64_t get_wide(const ConstPropEnvironment& env, int16_t first_reg) {
    assert(is_wide_constant(env, first_reg));
    const auto& domain1 = env.get(first_reg);
    const auto& domain2 = env.get(first_reg + 1);

    int64_t result =
        static_cast<int64_t>(env.get(first_reg).value().constant()) &
        0xFFFFFFFFL;
    result <<= 32;
    result |= static_cast<int64_t>(env.get(first_reg + 1).value().constant()) &
              0xFFFFFFFFL;
    return result;
  }
};

template <typename BlockType,
          typename InstructionType,
          typename BlockIterable,
          typename InstructionIterable>
class ConstantPropFixpointAnalysis
    : public MonotonicFixpointIterator<BlockType, ConstPropEnvironment> {
 public:
  using BlockTypeToListFunc =
      std::function<std::vector<BlockType>(BlockType const&)>;

  ConstantPropFixpointAnalysis(BlockType const& start_block,
                               BlockIterable const& cfg_iterable,
                               BlockTypeToListFunc succ,
                               BlockTypeToListFunc pred)
      : m_start_block(start_block),
        m_cfg_iterable(cfg_iterable),
        m_succ(succ),
        MonotonicFixpointIterator<BlockType, ConstPropEnvironment>(
            start_block, succ, pred) {}

  void simplify() const {
    for (const auto& block : m_cfg_iterable) {
      for (auto& insn : InstructionIterable(block)) {
        auto state = this->get_entry_state_at(block);
        analyze_instruction(insn, &state);
        simplify_instruction(block, insn, state);
      }
    }
  }

  ConstPropEnvironment analyze_edge(
      BlockType const& source,
      BlockType const& destination,
      const ConstPropEnvironment& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_node(BlockType const& block,
                    ConstPropEnvironment* state_at_entry) const override {
    for (auto& insn : InstructionIterable(block)) {
      analyze_instruction(insn, state_at_entry);
    }
  }

  ConstPropEnvironment get_constants_at_entry(BlockType const& node) const {
    return this->get_entry_state_at(node);
  }

  virtual void simplify_instruction(
      const BlockType& block,
      InstructionType& insn,
      const ConstPropEnvironment& current_state) const = 0;
  virtual void analyze_instruction(
      const InstructionType& insn,
      ConstPropEnvironment* current_state) const = 0;

 private:
  BlockType m_start_block;
  BlockIterable m_cfg_iterable;
  BlockTypeToListFunc m_succ;
};
