/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#if defined(_MSC_VER) || defined(__MINGW64__) || defined(__MINGW32__)

#include <stdint.h>
#include <BaseTsd.h>
#include <sys/types.h>

using ssize_t = SSIZE_T;
using off64_t = int64_t;
using off_t = _off_t;

#define __PRETTY_FUNCTION__ __func__

#endif // _MSC_VER
