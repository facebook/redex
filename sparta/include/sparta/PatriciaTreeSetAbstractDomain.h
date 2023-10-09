/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/PatriciaTreeSet.h>
#include <sparta/SetAbstractDomain.h>

namespace sparta {

/*
 * An implementation of powerset abstract domains using Patricia trees. This
 * implementation should be used for analyses that create large numbers of
 * identical or nearly identical sets (like a pointer analysis, for example).
 * This powerset domain can only handle elements that are either unsigned
 * integers or pointers to objects.
 *
 * Sample usage:
 *
 *  using Powerset = PatriciaTreeSetAbstractDomain<std::string*>;
 *
 *  std::string a = "a";
 *  ...
 *  Powerset s;
 *
 *  s.add(&a);
 *  ...
 *  for(std::string* p : s) {
 *    if (*p == "a") {
 *      ...
 *    }
 *  }
 *
 */
template <typename Element>
using PatriciaTreeSetAbstractDomain =
    SetAbstractDomain<PatriciaTreeSet<Element>>;

} // namespace sparta
