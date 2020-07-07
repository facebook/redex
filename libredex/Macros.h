/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifndef _MSC_VER

// Move to [[maybe_unused]] when switching to C++17.
#define ATTRIBUTE_UNUSED __attribute__((unused))

#else

// Windows would use some declspec. Don't care for now.
#define ATTRIBUTE_UNUSED

#endif
