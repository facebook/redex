/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */



#ifndef _FB_ATOMIC_REIMPLEMENTATION
#define _FB_ATOMIC_REIMPLEMENTATION

#include <atomic>
#include <stdint.h>
#include <sys/types.h>

static inline int32_t android_atomic_inc(volatile int32_t* addr) {
    volatile std::atomic_int_least32_t* a = (volatile std::atomic_int_least32_t*)addr;
        /* Int32_t, if it exists, is the same as int_least32_t. */
    return std::atomic_fetch_add_explicit(a, 1, std::memory_order_release);
}

static inline int32_t android_atomic_dec(volatile int32_t* addr) {
    volatile std::atomic_int_least32_t* a = (volatile std::atomic_int_least32_t*)addr;
    return std::atomic_fetch_sub_explicit(a, 1, std::memory_order_release);
}

#endif // _FB_ATOMIC_REIMPLEMENTATION
