/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApkResources.h"

#include "RedexResources.h"
#include <boost/optional.hpp>

boost::optional<int32_t> ApkResources::get_min_sdk() { return boost::none; }
