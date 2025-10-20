/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>

#if __cpp_lib_hardware_interference_size >= 201603

#include <new>

constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;

#else

// Fall back to something that matches x86 processors.
constexpr size_t CACHE_LINE_SIZE = 64;

#endif
