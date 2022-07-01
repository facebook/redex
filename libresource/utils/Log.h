/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */



#ifndef _FB_LOG_H_REIMPLEMENTATION
#define _FB_LOG_H_REIMPLEMENTATION

#define OS_PATH_SEPARATOR '/'

#include <cstdio>
#include <cstdlib>
// do {...} while(0) is a trick to allow multiple statements to be used in macros without messing up
// if statements and the like.
#define ALOGF(...) \
    do {fprintf(stderr, "FATAL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");} while(0)
#define ALOGE(...) \
    do {fprintf(stderr, "ERROR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");} while(0)
#define ALOGW(...) \
    do {fprintf(stderr, "WARNING: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");} while(0)
#define ALOGI(...) \
    do {fprintf(stderr, "INFO: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");} while(0)
#define ALOGD(...) \
    do {fprintf(stderr, "DEBUG: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");} while(0)
#define ALOGV(...) \
    do {fprintf(stderr, "VERBOSE: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");} while(0)

#define LOG_ALWAYS_FATAL(...) \
    do {ALOGF(__VA_ARGS__); abort();} while(0)

#define LOG_FATAL_IF(cond, ...) \
    do { if (cond) {ALOGF(#cond); ALOGF(__VA_ARGS__); abort();}} while(0)

#define LOG_ALWAYS_FATAL_IF(cond, ...) \
    LOG_FATAL_IF(cond, __VA_ARGS__)

#define ALOGW_IF(cond, ...) \
    do { if (cond) {ALOGW(#cond); ALOGW(__VA_ARGS__);}} while(0)

#define ALOG_ASSERT(cond, ...) \
    do { if (!(cond)) {ALOGF("Assertion failed"); ALOGF(#cond); ALOGF(__VA_ARGS__); abort();}} while(0)


#endif // _FB_LOG_H_REIMPLEMENTATION
