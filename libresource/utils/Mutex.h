/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _FB_MUTEX_REIMPLEMENTATION
#define _FB_MUTEX_REIMPLEMENTATION

#include <mutex>

using Mutex = std::mutex;
using AutoMutex = std::lock_guard<std::mutex>;

#endif // _FB_MUTEX_REIMPLEMENTATION
