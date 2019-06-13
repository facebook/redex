/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <iostream>
#include <sstream>
#include <utility>

#include "ConstantAbstractDomain.h"
#include "Debug.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"

/*
 * An abstract domain modeling an array that has a fixed, statically determined
 * size. It's a reduced product of a constant domain and a
 * PatriciaTreeMapAbstractEnvironment. It differs from a plain environment in
 * the following ways:
 *
 *   - Reading from an out-of-bounds index returns Bottom
 *   - Assigning to an out-of-bounds index causes the array to be set to Bottom
 *   - Top represents arrays of any size. If it is Top, any attempts to update
 *     its bindings are no-ops, since we cannot determine if our array reads
 *     and writes are within its bounds.
 */
template <typename Domain>
class ConstantArrayDomain final
    : public sparta::ReducedProductAbstractDomain<
          ConstantArrayDomain<Domain>,
          sparta::ConstantAbstractDomain<uint32_t> /* array length */,
          sparta::PatriciaTreeMapAbstractEnvironment<
              uint32_t,
              Domain> /* array values */> {
 public:
  using ArrayLengthDomain = sparta::ConstantAbstractDomain<uint32_t>;
  using ArrayValuesDomain =
      sparta::PatriciaTreeMapAbstractEnvironment<uint32_t, Domain>;
  using SuperType =
      sparta::ReducedProductAbstractDomain<ConstantArrayDomain<Domain>,
                                           ArrayLengthDomain,
                                           ArrayValuesDomain>;
  using typename SuperType::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
  ConstantArrayDomain() = default;

  static void reduce_product(
      std::tuple<ArrayLengthDomain, ArrayValuesDomain>& domains) {}

  ~ConstantArrayDomain() override {
    // The destructor is the only method that is guaranteed to be created when
    // a class template is instantiated. This is a good place to perform all
    // the sanity checks on the template parameters.
    static_assert(
        std::is_same<decltype(Domain::default_value()), Domain>::value,
        "Domain::default_value() does not exist");
  }

  ConstantArrayDomain(uint32_t length) {
    mutate_array_length(
        [length](ArrayLengthDomain* len) { *len = ArrayLengthDomain(length); });
    mutate_array_values([length](ArrayValuesDomain* values) {
      // default_value should typically be something representing zero, since
      // Java arrays are zero-initialized.
      for (size_t i = 0; i < length; ++i) {
        values->set(i, Domain::default_value());
      }
    });
    canonicalize();
  }

  void join_with(const ConstantArrayDomain<Domain>& other_domain) override {
    SuperType::join_with(other_domain);
    canonicalize();
  }

  void widen_with(const ConstantArrayDomain<Domain>& other_domain) override {
    SuperType::widen_with(other_domain);
    canonicalize();
  }

  bool is_value() const { return !this->is_top() && !this->is_bottom(); }

  uint32_t length() const {
    auto len = array_length();
    redex_assert(len.is_value());
    return *len.get_constant();
  }

  // NOTE: This will throw if array_values() is Top.
  const typename sparta::PatriciaTreeMapAbstractEnvironment<uint32_t,
                                                            Domain>::MapType&
  bindings() const {
    return array_values().bindings();
  }

  Domain get(uint32_t idx) const {
    if (this->is_top()) {
      return Domain::top();
    }
    if (this->is_bottom() || !(idx < this->length())) {
      return Domain::bottom();
    }
    return array_values().get(idx);
  }

  ConstantArrayDomain& set(uint32_t idx, const Domain& value) {
    if (!is_value()) {
      return *this;
    }
    if (!(idx < length())) {
      this->set_to_bottom();
      return *this;
    }
    return this->mutate_array_values(
        [&](ArrayValuesDomain* values) { values->set(idx, value); });
  }

  ConstantArrayDomain& update(uint32_t idx,
                              std::function<Domain(const Domain&)> operation) {
    if (!is_value()) {
      return *this;
    }
    if (!(idx < this->length())) {
      this->set_to_bottom();
      return *this;
    }
    this->mutate_array_values(
        [&](ArrayValuesDomain* values) { values->update(idx, operation); });
    return *this;
  }

  std::string str() const;

 private:
  // We have defined another get() method above, so this `using` declaration
  // is necessary to make it clear that the calls in array_length() and
  // array_values() below are referring to the superclass' implementation.
  using SuperType::get;

  const ArrayLengthDomain& array_length() const {
    return this->template get<0>();
  }

  const ArrayValuesDomain& array_values() const {
    return this->template get<1>();
  }

  ConstantArrayDomain<Domain>& mutate_array_length(
      std::function<void(ArrayLengthDomain*)> f) {
    this->template apply<0>(f);
    canonicalize();
    return *this;
  }

  ConstantArrayDomain<Domain>& mutate_array_values(
      std::function<void(ArrayValuesDomain*)> f) {
    this->template apply<1>(f);
    return *this;
  }

  void canonicalize() {
    // If we have an array of unknown length, we can't say anything about its
    // values either -- we don't know if a given read or write to the array
    // is going to throw an OOB exception.
    if (array_length().is_top()) {
      mutate_array_values(
          [](ArrayValuesDomain* values) { values->set_to_top(); });
    }
  }
};

template <typename Domain>
inline std::ostream& operator<<(std::ostream& o,
                                const ConstantArrayDomain<Domain>& e) {
  if (e.is_bottom()) {
    o << "_|_";
    return o;
  }
  if (e.is_top()) {
    o << "T";
    return o;
  }

  o << "[#" << e.length() << "]";
  o << "{";
  auto& bindings = e.bindings();
  for (size_t i = 0; i < e.length();) {
    o << bindings.at(i);
    ++i;
    if (i != e.length()) {
      o << ", ";
    }
  }
  o << "}";
  return o;
}

template <typename Domain>
inline std::string ConstantArrayDomain<Domain>::str() const {
  std::ostringstream ss;
  ss << *this;
  return ss.str();
}
