/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _FB_ANDROID_SERIALIZE_H
#define _FB_ANDROID_SERIALIZE_H

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Debug.h"
#include "utils/Log.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/TypeHelpers.h"
#include "utils/Unicode.h"
#include "utils/Vector.h"

namespace arsc {

void align_vec(size_t s, android::Vector<char>* vec);
void push_short(uint16_t data, android::Vector<char>* vec);
void push_long(uint32_t data, android::Vector<char>* vec);
void push_u8_length(size_t len, android::Vector<char>* vec);
void encode_string8(const android::String8& s, android::Vector<char>* vec);
void encode_string16(const android::String16& s, android::Vector<char>* vec);

} // namespace arsc
#endif
