/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ostream>
#include <sstream>
#include <type_traits>
#include <utility>

#include <boost/optional.hpp>

#include "AbstractDomain.h"

namespace sparta {

/*
 * This abstract domain combinator constructs the lattice of constants of a
 * certain type (also called the flat lattice or the three-level lattice). For
 * more detail on constant propagation please see:
 *
 *   https://www.cs.utexas.edu/users/lin/cs380c/wegman.pdf
 *
 * For example, the lattice of integer constants:
 *
 *                       TOP
 *                     /  |  \
 *           ... -2  -1   0   1  2 ....
 *                    \   |   /
 *                       _|_
 *
 * can be implemented as follows:
 *
 *   using Int32ConstantDomain = ConstantAbstractDomain<int32_t>;
 *
 * Note: The base constant elements should be comparable using `operator==()`.
 */

namespace acd_impl {

template <typename Constant>
class ConstantAbstractValue final
    : public AbstractValue<ConstantAbstractValue<Constant>> {
 public:
  ~ConstantAbstractValue() {
    static_assert(std::is_default_constructible<Constant>::value,
                  "Constant is not default constructible");
    static_assert(std::is_copy_constructible<Constant>::value,
                  "Constant is not copy constructible");
    static_assert(std::is_copy_assignable<Constant>::value,
                  "Constant is not copy assignable");
  }

  ConstantAbstractValue() = default;

  explicit ConstantAbstractValue(const Constant& constant)
      : m_constant(constant) {}

  void clear() override {}

  AbstractValueKind kind() const override { return AbstractValueKind::Value; }

  bool leq(const ConstantAbstractValue& other) const override {
    return equals(other);
  }

  bool equals(const ConstantAbstractValue& other) const override {
    return m_constant == other.get_constant();
  }

  AbstractValueKind join_with(const ConstantAbstractValue& other) override {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    return AbstractValueKind::Top;
  }

  AbstractValueKind widen_with(const ConstantAbstractValue& other) override {
    return join_with(other);
  }

  AbstractValueKind meet_with(const ConstantAbstractValue& other) override {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    return AbstractValueKind::Bottom;
  }

  AbstractValueKind narrow_with(const ConstantAbstractValue& other) override {
    return meet_with(other);
  }

  const Constant& get_constant() const { return m_constant; }

 private:
  Constant m_constant;
};

} // namespace acd_impl

template <typename Constant>
class ConstantAbstractDomain final
    : public AbstractDomainScaffolding<
          acd_impl::ConstantAbstractValue<Constant>,
          ConstantAbstractDomain<Constant>> {
 public:
  using ConstantType = Constant;

  ConstantAbstractDomain() { this->set_to_top(); }

  explicit ConstantAbstractDomain(const Constant& cst) {
    this->set_to_value(acd_impl::ConstantAbstractValue<Constant>(cst));
  }

  explicit ConstantAbstractDomain(AbstractValueKind kind)
      : AbstractDomainScaffolding<acd_impl::ConstantAbstractValue<Constant>,
                                  ConstantAbstractDomain<Constant>>(kind) {}

  boost::optional<Constant> get_constant() const {
    return (this->kind() == AbstractValueKind::Value)
               ? boost::optional<Constant>(this->get_value()->get_constant())
               : boost::none;
  }

  static ConstantAbstractDomain bottom() {
    return ConstantAbstractDomain(AbstractValueKind::Bottom);
  }

  static ConstantAbstractDomain top() {
    return ConstantAbstractDomain(AbstractValueKind::Top);
  }

  friend std::ostream& operator<<(
      std::ostream& out,
      const typename sparta::ConstantAbstractDomain<Constant>& x) {
    using namespace sparta;
    switch (x.kind()) {
    case AbstractValueKind::Bottom: {
      out << "_|_";
      break;
    }
    case AbstractValueKind::Top: {
      out << "T";
      break;
    }
    case AbstractValueKind::Value: {
      out << *x.get_constant();
      break;
    }
    }
    return out;
  }
};

} // namespace sparta
