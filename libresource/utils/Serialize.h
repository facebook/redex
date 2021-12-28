/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

 #ifndef _FB_ANDROID_SERIALIZE_H
 #define _FB_ANDROID_SERIALIZE_H

 #include "utils/ByteOrder.h"
 #include "utils/Debug.h"
 #include "utils/Log.h"
 #include "utils/String16.h"
 #include "utils/String8.h"
 #include "utils/Unicode.h"
 #include "utils/Vector.h"

namespace android {

void align_vec(android::Vector<char>& cVec, size_t s);
void push_short(android::Vector<char>& cVec, uint16_t data);
void push_long(android::Vector<char>& cVec, uint32_t data);
void push_u8_length(android::Vector<char>& cVec, size_t len);
void encode_string8(android::Vector<char>& cVec, android::String8 s);
void encode_string16(android::Vector<char>& cVec, android::String16 s);

}
#endif
