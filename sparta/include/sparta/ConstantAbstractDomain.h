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

#include <boost/intrusive/pointer_plus_bits.hpp>
#include <boost/optional.hpp>

#include <sparta/AbstractDomain.h>

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
  static_assert(std::is_default_constructible_v<Constant>,
                "Constant is not default constructible");
  static_assert(std::is_copy_constructible_v<Constant>,
                "Constant is not copy constructible");
  static_assert(std::is_copy_assignable_v<Constant>,
                "Constant is not copy assignable");

  ConstantAbstractValue() = default;

  explicit ConstantAbstractValue(Constant constant)
      : m_constant(std::move(constant)) {}

  void clear() {}

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool leq(const ConstantAbstractValue& other) const { return equals(other); }

  bool equals(const ConstantAbstractValue& other) const {
    return m_constant == other.get_constant();
  }

  AbstractValueKind join_with(const ConstantAbstractValue& other) {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    return AbstractValueKind::Top;
  }

  AbstractValueKind widen_with(const ConstantAbstractValue& other) {
    return join_with(other);
  }

  AbstractValueKind meet_with(const ConstantAbstractValue& other) {
    if (equals(other)) {
      return AbstractValueKind::Value;
    }
    return AbstractValueKind::Bottom;
  }

  AbstractValueKind narrow_with(const ConstantAbstractValue& other) {
    return meet_with(other);
  }

  const Constant& get_constant() const { return m_constant; }

 private:
  Constant m_constant;
};

template <typename Constant, typename = void>
class ConstantAbstractValueRepr {
 private:
  Constant m_constant;
  AbstractValueKind m_kind;

 public:
  AbstractValueKind kind() const { return m_kind; }
  const Constant& constant() const { return m_constant; }

  void set(AbstractValueKind kind, Constant constant) {
    m_kind = kind;
    m_constant = std::move(constant);
  }
  void set(AbstractValueKind kind) { m_kind = kind; }
};

template <typename Constant>
class ConstantAbstractValueRepr<
    Constant,
    std::enable_if_t<std::is_pointer<Constant>::value &&
                     boost::intrusive::max_pointer_plus_bits<
                         void*,
                         std::alignment_of<typename std::remove_pointer<
                             Constant>::type>::value>::value >= 2>> {
 private:
  using pointer_plus_bits = boost::intrusive::pointer_plus_bits<Constant, 2>;
  Constant m_constant;

 public:
  static_assert(static_cast<size_t>(AbstractValueKind::Bottom) < 4,
                "AbstractValueKind doesn't fit into 2 bits");
  static_assert(static_cast<size_t>(AbstractValueKind::Value) < 4,
                "AbstractValueKind doesn't fit into 2 bits");
  static_assert(static_cast<size_t>(AbstractValueKind::Top) < 4,
                "AbstractValueKind doesn't fit into 2 bits");

  AbstractValueKind kind() const {
    return static_cast<AbstractValueKind>(
        pointer_plus_bits::get_bits(m_constant));
  }
  Constant constant() const {
    return pointer_plus_bits::get_pointer(m_constant);
  }

  void set(AbstractValueKind kind, Constant constant) {
    m_constant = constant;
    pointer_plus_bits::set_bits(m_constant, static_cast<size_t>(kind));
  }
  void set(AbstractValueKind kind) {
    pointer_plus_bits::set_bits(m_constant, static_cast<size_t>(kind));
  }
};

} // namespace acd_impl

template <typename Constant>
class ConstantAbstractDomain final
    : public AbstractDomain<ConstantAbstractDomain<Constant>> {
 public:
  using ReprType = acd_impl::ConstantAbstractValueRepr<Constant>;
  using ConstantType = Constant;

  ConstantAbstractDomain() { m_repr.set(AbstractValueKind::Top); }

  explicit ConstantAbstractDomain(Constant cst) {
    m_repr.set(AbstractValueKind::Value, std::move(cst));
  }

  /*
   * A convenience constructor for creating Bottom and Top.
   */
  explicit ConstantAbstractDomain(AbstractValueKind kind) {
    m_repr.set(kind);
    SPARTA_RUNTIME_CHECK(kind != AbstractValueKind::Value,
                         invalid_abstract_value() << actual_kind(kind));
  }

  boost::optional<Constant> get_constant() const {
    return (m_repr.kind() == AbstractValueKind::Value)
               ? boost::optional<Constant>(m_repr.constant())
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

  AbstractValueKind kind() const { return m_repr.kind(); }

  bool is_bottom() const { return m_repr.kind() == AbstractValueKind::Bottom; }

  bool is_top() const { return m_repr.kind() == AbstractValueKind::Top; }

  bool is_value() const { return m_repr.kind() == AbstractValueKind::Value; }

  void set_to_bottom() { m_repr.set(AbstractValueKind::Bottom); }

  void set_to_top() { m_repr.set(AbstractValueKind::Top); }

  bool leq(const ConstantAbstractDomain<Constant>& other) const {
    if (is_bottom()) {
      return true;
    }
    if (other.is_bottom()) {
      return false;
    }
    if (other.is_top()) {
      return true;
    }
    if (is_top()) {
      return false;
    }
    return m_repr.constant() == other.m_repr.constant();
  }

  bool equals(const ConstantAbstractDomain<Constant>& other) const {
    if (is_bottom()) {
      return other.is_bottom();
    }
    if (is_top()) {
      return other.is_top();
    }
    if (!other.is_value()) {
      return false;
    }
    return m_repr.constant() == other.m_repr.constant();
  }

  void join_with(const ConstantAbstractDomain<Constant>& other) {
    if (is_top() || other.is_bottom()) {
      return;
    }
    if (other.is_top()) {
      set_to_top();
      return;
    }
    if (is_bottom()) {
      m_repr = other.m_repr;
      return;
    }
    m_repr.set(m_repr.constant() == other.m_repr.constant()
                   ? AbstractValueKind::Value
                   : AbstractValueKind::Top);
  }

  void widen_with(const ConstantAbstractDomain<Constant>& other) {
    return join_with(other);
  }

  void meet_with(const ConstantAbstractDomain<Constant>& other) {
    if (is_bottom() || other.is_top()) {
      return;
    }
    if (other.is_bottom()) {
      set_to_bottom();
      return;
    }
    if (is_top()) {
      m_repr = other.m_repr;
      return;
    }
    m_repr.set(m_repr.constant() == other.m_repr.constant()
                   ? AbstractValueKind::Value
                   : AbstractValueKind::Bottom);
  }

  void narrow_with(const ConstantAbstractDomain<Constant>& other) {
    return meet_with(other);
  }

 private:
  ReprType m_repr;
};

} // namespace sparta
