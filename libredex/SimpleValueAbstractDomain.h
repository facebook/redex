/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    : public sparta::AbstractDomainScaffolding<svad_impl::SimpleValue<T>,
                                               SimpleValueAbstractDomain<T>> {
 public:
  static SimpleValueAbstractDomain<T> value(T value) {
    SimpleValueAbstractDomain<T> result;
    result.set_to_value(svad_impl::SimpleValue<T>(value));
    return result;
  }

  SimpleValueAbstractDomain<T>(
      sparta::AbstractValueKind kind = sparta::AbstractValueKind::Top)
      : sparta::AbstractDomainScaffolding<svad_impl::SimpleValue<T>,
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
class SimpleValue final : public sparta::AbstractValue<SimpleValue<T>> {
 public:
  friend class SimpleValueAbstractDomain<T>;

  void clear() override{};

  sparta::AbstractValueKind kind() const override {
    return sparta::AbstractValueKind::Value;
  }

  bool equals(const SimpleValue<T>& other) const override {
    return m_value == other.m_value;
  }

  bool leq(const SimpleValue<T>& other) const override { return equals(other); }

  sparta::AbstractValueKind join_with(const SimpleValue<T>& other) override {
    using namespace sparta;
    if (!equals(other)) {
      return AbstractValueKind::Top;
    } else {
      return AbstractValueKind::Value;
    }
  }

  sparta::AbstractValueKind meet_with(const SimpleValue<T>& other) override {
    using namespace sparta;
    if (!equals(other)) {
      return AbstractValueKind::Bottom;
    } else {
      return AbstractValueKind::Value;
    }
  }

  sparta::AbstractValueKind widen_with(const SimpleValue<T>& other) override {
    return join_with(other);
  }

  sparta::AbstractValueKind narrow_with(const SimpleValue<T>& other) override {
    return meet_with(other);
  }

  T value() const { return m_value; }

  SimpleValue<T>() {}
  SimpleValue<T>(T value) : m_value(value) {}

 private:
  T m_value;
};
} // namespace svad_impl
