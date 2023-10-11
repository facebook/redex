/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/AbstractEnvironment.h>
#include <sparta/HashMap.h>

namespace sparta {

/*
 * An abstract environment is a type of abstract domain that maps the variables
 * of a program to elements of a common abstract domain. For example, to perform
 * range analysis one can use an abstract environment that maps variable names
 * to intervals:
 *
 *   {"x" -> [-1, 1], "i" -> [0, 10], ...}
 *
 * Another example is descriptive type analysis for Dex code, where one computes
 * the set of all possible Java classes a register can hold a reference to at
 * any point in the code:
 *
 *  {"v0" -> {android.app.Fragment, java.lang.Object}, "v1" -> {...}, ...}
 *
 * This type of domain is commonly used for nonrelational (also called
 * attribute-independent) analyses that do not track relationships among
 * program variables. Please note that by definition of an abstract
 * environment, if the value _|_ appears in a variable binding, then no valid
 * execution state can ever be represented by this abstract environment. Hence,
 * assigning _|_ to a variable is equivalent to setting the entire environment
 * to _|_.
 *
 * This implementation of abstract environments is based on hashtables and is
 * well suited for intraprocedural analysis. It is not intended to handle very
 * large variable sets in the thousands. We use the AbstractDomainScaffolding
 * template to build the domain. In order to minimize the size of the underlying
 * hashtable, we do not explicitly represent bindings of a variable to the Top
 * element. Hence, any variable that is not explicitly represented in the
 * environment has a default value of Top. This representation is quite
 * convenient in practice. It also allows us to manipulate large (or possibly
 * infinite) variable sets with sparse assignments of non-Top values.
 */
template <typename Variable,
          typename Domain,
          typename VariableHash = std::hash<Variable>,
          typename VariableEqual = std::equal_to<Variable>>
using HashedAbstractEnvironment =
    AbstractEnvironment<HashMap<Variable,
                                Domain,
                                TopValueInterface<Domain>,
                                VariableHash,
                                VariableEqual>>;

} // namespace sparta
