/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"

namespace class_merging {

namespace type_tag_utils {

/**
 * Parse `typeTag` fields on the @ModelIdentity annotation of a given class.
 */
boost::optional<uint32_t> parse_model_type_tag(const DexClass*);

} // namespace type_tag_utils

} // namespace class_merging
