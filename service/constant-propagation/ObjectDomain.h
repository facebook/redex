/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/functional/hash.hpp>

#include "DexClass.h"
#include "FiniteAbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"

/*
 * ObjectDomain is an abstract environment coupled with logic that tracks
 * whether the represented object has escaped. Escaped objects are represented
 * by the Top element, and cannot be updated -- since we do not know if the
 * object is being concurrently modified, we cannot conclude anything about
 * the values of its fields.
 */

enum class EscapeState {
  MAY_ESCAPE,
  NOT_ESCAPED,
  BOTTOM,
};

namespace escape_domain_impl {

using Lattice = BitVectorLattice<EscapeState,
                                 /* cardinality */ 3,
                                 boost::hash<EscapeState>>;

extern Lattice lattice;

using Domain =
    FiniteAbstractDomain<EscapeState, Lattice, Lattice::Encoding, &lattice>;

} // namespace escape_domain_impl

using EscapeDomain = escape_domain_impl::Domain;

std::ostream& operator<<(std::ostream& os, const EscapeDomain& dom);

template <typename FieldValue>
class ObjectDomain final
    : public ReducedProductAbstractDomain<
          ObjectDomain<FieldValue>,
          EscapeDomain,
          PatriciaTreeMapAbstractEnvironment<const DexField*, FieldValue>> {

 public:
  using FieldEnvironment =
      PatriciaTreeMapAbstractEnvironment<const DexField*, FieldValue>;

  using Base = ReducedProductAbstractDomain<ObjectDomain<FieldValue>,
                                            EscapeDomain,
                                            FieldEnvironment>;

  using Base::Base;

  /*
   * The default constructor produces a non-escaping empty object.
   */
  ObjectDomain()
      : Base(std::make_tuple(EscapeDomain(EscapeState::NOT_ESCAPED),
                             FieldEnvironment())) {}

  static void reduce_product(std::tuple<EscapeDomain, FieldEnvironment>& doms) {
    if (std::get<0>(doms).element() == EscapeState::MAY_ESCAPE) {
      std::get<1>(doms).set_to_top();
    }
  }

  ObjectDomain& set(const DexField* field, const FieldValue& value) {
    if (this->is_escaped()) {
      return *this;
    }
    this->template apply<1>(
        [&](FieldEnvironment* env) { env->set(field, value); });
    return *this;
  }

  FieldValue get(const DexField* field) {
    return get_field_environment().get(field);
  }

  template <typename Domain>
  Domain get(const DexField* field) {
    return get_field_environment().get(field).template get<Domain>();
  }

  bool is_escaped() const {
    return this->get_escape_domain().element() == EscapeState::MAY_ESCAPE;
  }

  void set_escaped() { this->set_to_top(); }

 private:
  const EscapeDomain& get_escape_domain() const {
    return this->Base::template get<0>();
  }

  const FieldEnvironment& get_field_environment() const {
    return this->Base::template get<1>();
  }
};
