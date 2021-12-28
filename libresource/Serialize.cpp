/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

 #include "utils/ByteOrder.h"
 #include "utils/Debug.h"
 #include "utils/Log.h"
 #include "utils/String16.h"
 #include "utils/String8.h"
 #include "utils/Unicode.h"
 #include "utils/Vector.h"

 namespace android {

 void align_vec(android::Vector<char>& cVec, size_t s) {
   size_t r = cVec.size() % s;
   if (r > 0) {
     for (size_t i = s - r; i > 0; i--) {
       cVec.push_back(0);
     }
   }
 }

 void push_short(android::Vector<char>& cVec, uint16_t data) {
   auto swapped = htods(data);
   cVec.push_back(swapped);
   cVec.push_back(swapped >> 8);
 }

 void push_long(android::Vector<char>& cVec, uint32_t data) {
   auto swapped = htodl(data);
   cVec.push_back(swapped);
   cVec.push_back(swapped >> 8);
   cVec.push_back(swapped >> 16);
   cVec.push_back(swapped >> 24);
 }

 void push_u8_length(android::Vector<char>& cVec, size_t len) {
   // If len > 2^7-1, then set the most significant bit, then use a second byte
   // to describe the length (leaving 15 bits for the actual len).
   if (len >= 0x80) {
     const auto mask = 0x8000;
     LOG_FATAL_IF(len >= mask, "String length too large");
     // Set the high bit, then push it in two pieces (can't just push short).
     uint16_t encoded = mask | len;
     uint8_t high = encoded >> 8;
     uint8_t low = encoded & 0xFF;
     cVec.push_back(high);
     cVec.push_back(low);
   } else {
     cVec.push_back((char) len);
   }
 }

 void encode_string8(android::Vector<char>& cVec, android::String8 s) {
   // aapt2 writes both the utf16 length followed by utf8 length
   size_t len = s.length();
   const uint8_t* u8_str = (const uint8_t*) s.string();
   auto u16_len = utf8_to_utf16_length(u8_str, len);
   push_u8_length(cVec, u16_len);
   push_u8_length(cVec, len);
   // Push each char
   for (uint8_t* c = (uint8_t*) u8_str; *c; c++) {
     cVec.push_back(*c);
   }
   cVec.push_back('\0');
 }

 void encode_string16(android::Vector<char>& cVec, android::String16 s) {
   // Push uint16_t (2 bytes) describing the length. If length > 2^15-1, then set
   // most significant bit, then use two uint16_t to describe the length (first
   // uint16_t will be the high word).
   auto len = s.size();
   if (len >= 0x8000) {
     const auto mask = 0x80000000;
     LOG_FATAL_IF(len >= mask, "String length too large");
     uint32_t encoded = mask | len;
     push_short(cVec, encoded >> 16);
     push_short(cVec, encoded & 0xFFFF);
   } else {
     push_short(cVec, (uint16_t)len);
   }
   auto u16_str = s.string();
   for (uint16_t* c = (uint16_t*)u16_str; *c; c++) {
     push_short(cVec, *c);
   }
   push_short(cVec, '\0');
 }

} // namespace android
