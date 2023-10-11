/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/AbstractPartition.h>
#include <sparta/PatriciaTreeMap.h>

namespace sparta {

/*
 * An abstract partition based on Patricia trees that is cheap to copy.
 *
 * In order to minimize the size of the underlying tree, we do not explicitly
 * represent bindings of a label to the Bottom element.
 *
 * See AbstractPartition.h for more details about abstract partitions.
 *
 * This implementation differs slightly from the textbook definition of a
 * partition: our Top partition cannot have its labels re-bound to anything
 * other than Top. I.e. for all labels L and domains D,
 *
 *   PatriciaTreeMapAbstractPartition::top().set(L, D) ==
 * PatriciaTreeMapAbstractPartition::top()
 *
 * This makes for a much simpler implementation.
 */
template <typename Label, typename Domain>
using PatriciaTreeMapAbstractPartition = AbstractPartition<
    PatriciaTreeMap<Label, Domain, BottomValueInterface<Domain>>>;

} // namespace sparta
