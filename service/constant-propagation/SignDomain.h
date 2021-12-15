/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>

#include "FiniteAbstractDomain.h"

/*
 * This module deals with the signedness of integer types, representing them
 * as intervals with zero as an endpoint.
 */
namespace sign_domain {

enum class Interval {
  EMPTY, // Ø -- Bottom type
  LTZ, // (-∞, 0)
  GTZ, // (0, ∞)
  EQZ, // {0}
  NEZ, // Anything but 0
  GEZ, // [0, ∞)
  LEZ, // (-∞, 0]
  ALL, // (-∞, +∞) -- Top type

  SIZE // The number of items in Interval
};

using Lattice = sparta::BitVectorLattice<Interval,
                                         static_cast<size_t>(Interval::SIZE),
                                         boost::hash<Interval>>;

extern Lattice lattice;

// join and meet are the equivalent of interval union and intersection
// respectively.
using Domain = sparta::
    FiniteAbstractDomain<Interval, Lattice, Lattice::Encoding, &lattice>;

std::ostream& operator<<(std::ostream&, Interval);

std::ostream& operator<<(std::ostream&, const Domain&);

Domain from_int(int64_t);

bool contains(Interval, int64_t);

int64_t max_int(Interval interval);

int64_t min_int(Interval interval);

} // namespace sign_domain
