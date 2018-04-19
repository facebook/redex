/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "AbstractDomain.h"

namespace svad_impl {

template <typename T>
class SimpleValue;

} // namespace svad_impl

/**
 * This is a simple abstract domain that corresponds to having concrete values
 * of type T with the additional possibility of top and bottom.  It is a helper
 * class so one can create a domain with:
 * `using EasyDomain = SimpleValueAbstractDomain<simple_type>;`
 *
 * This class represents constant values living in the following lattice:
 *                           T ( All values possible )
 *
 *                  /        |         \
 *
 *                  [Concrete T values]
 *
 *                  \        |        /
 *
 *                          _|_ ( Invalid configuration )
 */
template <typename T>
class SimpleValueAbstractDomain
    : public AbstractDomainScaffolding<svad_impl::SimpleValue<T>,
                                       SimpleValueAbstractDomain<T>> {
 public:
  static SimpleValueAbstractDomain<T> bottom() {
    return SimpleValueAbstractDomain(AbstractValueKind::Bottom);
  }

  static SimpleValueAbstractDomain<T> top() {
    return SimpleValueAbstractDomain<T>(AbstractValueKind::Top);
  }

  static SimpleValueAbstractDomain<T> value(T value) {
    SimpleValueAbstractDomain<T> result;
    result.set_to_value(svad_impl::SimpleValue<T>(value));
    return result;
  }

  SimpleValueAbstractDomain<T>(AbstractValueKind kind = AbstractValueKind::Top)
      : AbstractDomainScaffolding<svad_impl::SimpleValue<T>,
                                  SimpleValueAbstractDomain<T>>(kind) {}

  T value() const { return this->get_value()->value(); }
};

template <typename T>
inline std::ostream& operator<<(std::ostream& o,
                                const SimpleValueAbstractDomain<T>& sv) {
  if (sv.is_bottom()) {
    o << "_|_";
  } else if (sv.is_top()) {
    o << "T";
  } else {
    o << sv.value();
  }
  return o;
}

namespace svad_impl {

template <typename T>
class SimpleValue final : public AbstractValue<SimpleValue<T>> {
 public:
  friend class SimpleValueAbstractDomain<T>;

  void clear() override{};

  AbstractValueKind kind() const override { return AbstractValueKind::Value; }

  bool equals(const SimpleValue<T>& other) const override {
    return m_value == other.m_value;
  }

  bool leq(const SimpleValue<T>& other) const override { return equals(other); }

  AbstractValueKind join_with(const SimpleValue<T>& other) override {
    if (!equals(other)) {
      return AbstractValueKind::Top;
    } else {
      return AbstractValueKind::Value;
    }
  }

  AbstractValueKind meet_with(const SimpleValue<T>& other) override {
    if (!equals(other)) {
      return AbstractValueKind::Bottom;
    } else {
      return AbstractValueKind::Value;
    }
  }

  AbstractValueKind widen_with(const SimpleValue<T>& other) override {
    return join_with(other);
  }

  AbstractValueKind narrow_with(const SimpleValue<T>& other) override {
    return meet_with(other);
  }

  T value() const { return m_value; }

  SimpleValue<T>() {}
  SimpleValue<T>(T value) : m_value(value) {}

 private:
  T m_value;
};
} // namespace svad_impl
