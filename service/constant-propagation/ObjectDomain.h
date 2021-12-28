/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
  // An object which escapes iff its originating parameter does
  ONLY_PARAMETER_DEPENDENT,
  NOT_ESCAPED,
  BOTTOM,
};

namespace escape_domain_impl {

using Lattice = sparta::BitVectorLattice<EscapeState,
                                         /* cardinality */ 4,
                                         boost::hash<EscapeState>>;

extern Lattice lattice;

using Domain = sparta::
    FiniteAbstractDomain<EscapeState, Lattice, Lattice::Encoding, &lattice>;

} // namespace escape_domain_impl

using EscapeDomain = escape_domain_impl::Domain;

std::ostream& operator<<(std::ostream& os, const EscapeDomain& dom);

template <typename FieldValue>
class ObjectDomain final
    : public sparta::ReducedProductAbstractDomain<
          ObjectDomain<FieldValue>,
          EscapeDomain,
          sparta::PatriciaTreeMapAbstractEnvironment<const DexField*,
                                                     FieldValue>> {

 public:
  using FieldEnvironment =
      sparta::PatriciaTreeMapAbstractEnvironment<const DexField*, FieldValue>;

  using Base = sparta::ReducedProductAbstractDomain<ObjectDomain<FieldValue>,
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
