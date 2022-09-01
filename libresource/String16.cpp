/*
 * Copyright (C) 2005 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utils/String16.h"

#include "utils/Log.h"

#include <ctype.h>

#include "utils/SharedBuffer.h"

namespace android {

static const StaticString16 emptyString(u"");
static inline char16_t* getEmptyString() {
    return const_cast<char16_t*>(emptyString.string());
}

// ---------------------------------------------------------------------------

void* String16::alloc(size_t size)
{
    SharedBuffer* buf = SharedBuffer::alloc(size);
    buf->mClientMetadata = kIsSharedBufferAllocated;
    return buf;
}

char16_t* String16::allocFromUTF8(const char* u8str, size_t u8len)
{
    if (u8len == 0) return getEmptyString();

    const uint8_t* u8cur = (const uint8_t*) u8str;

    const ssize_t u16len = utf8_to_utf16_length(u8cur, u8len);
    if (u16len < 0) {
        return getEmptyString();
    }

    SharedBuffer* buf = static_cast<SharedBuffer*>(alloc(sizeof(char16_t) * (u16len + 1)));
    if (buf) {
        u8cur = (const uint8_t*) u8str;
        char16_t* u16str = (char16_t*)buf->data();

        utf8_to_utf16(u8cur, u8len, u16str, ((size_t) u16len) + 1);

        //printf("Created UTF-16 string from UTF-8 \"%s\":", in);
        //printHexData(1, str, buf->size(), 16, 1);
        //printf("\n");

        return u16str;
    }

    return getEmptyString();
}

char16_t* String16::allocFromUTF16(const char16_t* u16str, size_t u16len) {
    if (u16len >= SIZE_MAX / sizeof(char16_t)) {
        android_errorWriteLog(0x534e4554, "73826242");
        abort();
    }

    SharedBuffer* buf = static_cast<SharedBuffer*>(alloc((u16len + 1) * sizeof(char16_t)));
    ALOG_ASSERT(buf, "Unable to allocate shared buffer");
    if (buf) {
        char16_t* str = (char16_t*)buf->data();
        memcpy(str, u16str, u16len * sizeof(char16_t));
        str[u16len] = 0;
        return str;
    }
    return getEmptyString();
}

// ---------------------------------------------------------------------------

String16::String16()
    : mString(getEmptyString())
{
}

String16::String16(StaticLinkage)
    : mString(nullptr)
{
    // this constructor is used when we can't rely on the static-initializers
    // having run. In this case we always allocate an empty string. It's less
    // efficient than using getEmptyString(), but we assume it's uncommon.

    SharedBuffer* buf = static_cast<SharedBuffer*>(alloc(sizeof(char16_t)));
    char16_t* data = static_cast<char16_t*>(buf->data());
    data[0] = 0;
    mString = data;
}

String16::String16(const String16& o)
    : mString(o.mString)
{
    acquire();
}

String16::String16(const String16& o, size_t len, size_t begin)
    : mString(getEmptyString())
{
    setTo(o, len, begin);
}

String16::String16(const char16_t* o) : mString(allocFromUTF16(o, strlen16(o))) {}

String16::String16(const char16_t* o, size_t len) : mString(allocFromUTF16(o, len)) {}

String16::String16(const String8& o)
    : mString(allocFromUTF8(o.string(), o.size()))
{
}

String16::String16(const char* o)
    : mString(allocFromUTF8(o, strlen(o)))
{
}

String16::String16(const char* o, size_t len)
    : mString(allocFromUTF8(o, len))
{
}

String16::~String16()
{
    release();
}

size_t String16::size() const
{
    if (isStaticString()) {
        return staticStringSize();
    } else {
        return SharedBuffer::sizeFromData(mString) / sizeof(char16_t) - 1;
    }
}

void String16::setTo(const String16& other)
{
    release();
    mString = other.mString;
    acquire();
}

status_t String16::setTo(const String16& other, size_t len, size_t begin)
{
    const size_t N = other.size();
    if (begin >= N) {
        release();
        mString = getEmptyString();
        return OK;
    }
    if ((begin+len) > N) len = N-begin;
    if (begin == 0 && len == N) {
        setTo(other);
        return OK;
    }

    if (&other == this) {
        LOG_ALWAYS_FATAL("Not implemented");
    }

    return setTo(other.string()+begin, len);
}

status_t String16::setTo(const char16_t* other)
{
    return setTo(other, strlen16(other));
}

status_t String16::setTo(const char16_t* other, size_t len)
{
    if (len >= SIZE_MAX / sizeof(char16_t)) {
        android_errorWriteLog(0x534e4554, "73826242");
        abort();
    }

    SharedBuffer* buf = static_cast<SharedBuffer*>(editResize((len + 1) * sizeof(char16_t)));
    if (buf) {
        char16_t* str = (char16_t*)buf->data();
        memmove(str, other, len*sizeof(char16_t));
        str[len] = 0;
        mString = str;
        return OK;
    }
    return NO_MEMORY;
}

status_t String16::append(const String16& other)
{
    const size_t myLen = size();
    const size_t otherLen = other.size();
    if (myLen == 0) {
        setTo(other);
        return OK;
    } else if (otherLen == 0) {
        return OK;
    }

    if (myLen >= SIZE_MAX / sizeof(char16_t) - otherLen) {
        android_errorWriteLog(0x534e4554, "73826242");
        abort();
    }

    SharedBuffer* buf =
            static_cast<SharedBuffer*>(editResize((myLen + otherLen + 1) * sizeof(char16_t)));
    if (buf) {
        char16_t* str = (char16_t*)buf->data();
        memcpy(str+myLen, other, (otherLen+1)*sizeof(char16_t));
        mString = str;
        return OK;
    }
    return NO_MEMORY;
}

status_t String16::append(const char16_t* chrs, size_t otherLen)
{
    const size_t myLen = size();
    if (myLen == 0) {
        setTo(chrs, otherLen);
        return OK;
    } else if (otherLen == 0) {
        return OK;
    }

    if (myLen >= SIZE_MAX / sizeof(char16_t) - otherLen) {
        android_errorWriteLog(0x534e4554, "73826242");
        abort();
    }

    SharedBuffer* buf =
            static_cast<SharedBuffer*>(editResize((myLen + otherLen + 1) * sizeof(char16_t)));
    if (buf) {
        char16_t* str = (char16_t*)buf->data();
        memcpy(str+myLen, chrs, otherLen*sizeof(char16_t));
        str[myLen+otherLen] = 0;
        mString = str;
        return OK;
    }
    return NO_MEMORY;
}

status_t String16::insert(size_t pos, const char16_t* chrs)
{
    return insert(pos, chrs, strlen16(chrs));
}

status_t String16::insert(size_t pos, const char16_t* chrs, size_t len)
{
    const size_t myLen = size();
    if (myLen == 0) {
        return setTo(chrs, len);
        return OK;
    } else if (len == 0) {
        return OK;
    }

    if (pos > myLen) pos = myLen;

    #if 0
    printf("Insert in to %s: pos=%d, len=%d, myLen=%d, chrs=%s\n",
           String8(*this).string(), pos,
           len, myLen, String8(chrs, len).string());
    #endif

    SharedBuffer* buf =
            static_cast<SharedBuffer*>(editResize((myLen + len + 1) * sizeof(char16_t)));
    if (buf) {
        char16_t* str = (char16_t*)buf->data();
        if (pos < myLen) {
            memmove(str+pos+len, str+pos, (myLen-pos)*sizeof(char16_t));
        }
        memcpy(str+pos, chrs, len*sizeof(char16_t));
        str[myLen+len] = 0;
        mString = str;
        #if 0
        printf("Result (%d chrs): %s\n", size(), String8(*this).string());
        #endif
        return OK;
    }
    return NO_MEMORY;
}

ssize_t String16::findFirst(char16_t c) const
{
    const char16_t* str = string();
    const char16_t* p = str;
    const char16_t* e = p + size();
    while (p < e) {
        if (*p == c) {
            return p-str;
        }
        p++;
    }
    return -1;
}

ssize_t String16::findLast(char16_t c) const
{
    const char16_t* str = string();
    const char16_t* p = str;
    const char16_t* e = p + size();
    while (p < e) {
        e--;
        if (*e == c) {
            return e-str;
        }
    }
    return -1;
}

bool String16::startsWith(const String16& prefix) const
{
    const size_t ps = prefix.size();
    if (ps > size()) return false;
    return strzcmp16(mString, ps, prefix.string(), ps) == 0;
}

bool String16::startsWith(const char16_t* prefix) const
{
    const size_t ps = strlen16(prefix);
    if (ps > size()) return false;
    return strncmp16(mString, prefix, ps) == 0;
}

bool String16::contains(const char16_t* chrs) const
{
    return strstr16(mString, chrs) != nullptr;
}

void* String16::edit() {
    SharedBuffer* buf;
    if (isStaticString()) {
        buf = static_cast<SharedBuffer*>(alloc((size() + 1) * sizeof(char16_t)));
        if (buf) {
            memcpy(buf->data(), mString, (size() + 1) * sizeof(char16_t));
        }
    } else {
        buf = SharedBuffer::bufferFromData(mString)->edit();
        buf->mClientMetadata = kIsSharedBufferAllocated;
    }
    return buf;
}

void* String16::editResize(size_t newSize) {
    SharedBuffer* buf;
    if (isStaticString()) {
        size_t copySize = (size() + 1) * sizeof(char16_t);
        if (newSize < copySize) {
            copySize = newSize;
        }
        buf = static_cast<SharedBuffer*>(alloc(newSize));
        if (buf) {
            memcpy(buf->data(), mString, copySize);
        }
    } else {
        buf = SharedBuffer::bufferFromData(mString)->editResize(newSize);
        buf->mClientMetadata = kIsSharedBufferAllocated;
    }
    return buf;
}

void String16::acquire()
{
    if (!isStaticString()) {
        SharedBuffer::bufferFromData(mString)->acquire();
    }
}

void String16::release()
{
    if (!isStaticString()) {
        SharedBuffer::bufferFromData(mString)->release();
    }
}

bool String16::isStaticString() const {
    // See String16.h for notes on the memory layout of String16::StaticData and
    // SharedBuffer.
    static_assert(sizeof(SharedBuffer) - offsetof(SharedBuffer, mClientMetadata) == 4);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(mString);
    return (*(p - 1) & kIsSharedBufferAllocated) == 0;
}

size_t String16::staticStringSize() const {
    // See String16.h for notes on the memory layout of String16::StaticData and
    // SharedBuffer.
    static_assert(sizeof(SharedBuffer) - offsetof(SharedBuffer, mClientMetadata) == 4);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(mString);
    return static_cast<size_t>(*(p - 1));
}

status_t String16::makeLower()
{
    const size_t N = size();
    const char16_t* str = string();
    char16_t* edited = nullptr;
    for (size_t i=0; i<N; i++) {
        const char16_t v = str[i];
        if (v >= 'A' && v <= 'Z') {
            if (!edited) {
                SharedBuffer* buf = static_cast<SharedBuffer*>(edit());
                if (!buf) {
                    return NO_MEMORY;
                }
                edited = (char16_t*)buf->data();
                mString = str = edited;
            }
            edited[i] = tolower((char)v);
        }
    }
    return OK;
}

status_t String16::replaceAll(char16_t replaceThis, char16_t withThis)
{
    const size_t N = size();
    const char16_t* str = string();
    char16_t* edited = nullptr;
    for (size_t i=0; i<N; i++) {
        if (str[i] == replaceThis) {
            if (!edited) {
                SharedBuffer* buf = static_cast<SharedBuffer*>(edit());
                if (!buf) {
                    return NO_MEMORY;
                }
                edited = (char16_t*)buf->data();
                mString = str = edited;
            }
            edited[i] = withThis;
        }
    }
    return OK;
}

status_t String16::remove(size_t len, size_t begin)
{
    const size_t N = size();
    if (begin >= N) {
        release();
        mString = getEmptyString();
        return OK;
    }
    if (len > N || len > N - begin) len = N - begin;
    if (begin == 0 && len == N) {
        return OK;
    }

    if (begin > 0) {
        SharedBuffer* buf = static_cast<SharedBuffer*>(editResize((N + 1) * sizeof(char16_t)));
        if (!buf) {
            return NO_MEMORY;
        }
        char16_t* str = (char16_t*)buf->data();
        memmove(str, str+begin, (N-begin+1)*sizeof(char16_t));
        mString = str;
    }
    SharedBuffer* buf = static_cast<SharedBuffer*>(editResize((len + 1) * sizeof(char16_t)));
    if (buf) {
        char16_t* str = (char16_t*)buf->data();
        str[len] = 0;
        mString = str;
        return OK;
    }
    return NO_MEMORY;
}

}; // namespace android
