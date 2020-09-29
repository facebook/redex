/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
#define IS_WINDOWS 1
#else
#define IS_WINDOWS 0

// Windows requires O_BINARY to not interpret text files and
// have 0x1a terminate the file. For Linux, just make it `0`.
#define O_BINARY 0

#endif

#ifndef _MSC_VER

// Move to [[maybe_unused]] when switching to C++17.
#define ATTRIBUTE_UNUSED __attribute__((unused))

#else

// Windows would use some declspec. Don't care for now.
#define ATTRIBUTE_UNUSED

#endif
