/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#define ATTRIBUTE_UNUSED [[maybe_unused]]
#else
// Windows would use some declspec. Don't care for now.
#define ATTRIBUTE_UNUSED
#endif

#ifdef __clang__
#define ATTR_FORMAT(STR_INDEX, PARAM_INDEX) \
  __attribute__((__format__(__printf__, STR_INDEX, PARAM_INDEX)))
#elif defined(__GNUC__)
#define ATTR_FORMAT(STR_INDEX, PARAM_INDEX) \
  __attribute__((format(printf, STR_INDEX, PARAM_INDEX)));
#else
#define ATTR_FORMAT(...)
#endif
