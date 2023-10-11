/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/AbstractPartition.h>
#include <sparta/HashMap.h>

namespace sparta {

/*
 * A partition is a mapping from a set of labels to elements in an abstract
 * domain. It denotes a union of properties. A partition is Bottom iff all its
 * bindings are set to Bottom, and it is Top iff all its bindings are set to
 * Top.
 *
 * All lattice operations are applied componentwise.
 *
 * In order to minimize the size of the hashtable, we do not explicitly
 * represent bindings to Bottom.
 *
 * This implementation differs slightly from the textbook definition of a
 * partition: our Top partition cannot have its labels re-bound to anything
 * other than Top. I.e. for all labels L and domains D,
 *
 *   HashedAbstractPartition::top().set(L, D) == HashedAbstractPartition::top()
 *
 * This makes for a much simpler implementation.
 */
template <typename Label,
          typename Domain,
          typename LabelHash = std::hash<Label>,
          typename LabelEqual = std::equal_to<Label>>
using HashedAbstractPartition =
    AbstractPartition<HashMap<Label,
                              Domain,
                              BottomValueInterface<Domain>,
                              LabelHash,
                              LabelEqual>>;

} // namespace sparta
