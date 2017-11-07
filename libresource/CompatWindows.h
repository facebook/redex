/**
* Copyright (c) 2016-present, Facebook, Inc.
* All rights reserved.
*
* This source code is licensed under the BSD-style license found in the
* LICENSE file in the root directory of this source tree. An additional grant
* of patent rights can be found in the PATENTS file in the same directory.
*/

#pragma once

#if defined(_MSC_VER)

#include <stdint.h>
#include <BaseTsd.h>
#include <sys/types.h>

using ssize_t = SSIZE_T;
using off64_t = int64_t;
using off_t = _off_t;

#define __PRETTY_FUNCTION__ __func__

#endif // _MSC_VER
