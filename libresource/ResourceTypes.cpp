/*
 * Copyright (C) 2008 The Android Open Source Project
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

#define LOG_TAG "ResourceType"
//#define LOG_NDEBUG 0

#include <ctype.h>
#include <memory.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <limits>
#include <type_traits>

#if defined(_MSC_VER) || defined(__MINGW64__) || defined(__MINGW32__)
#include "CompatWindows.h"
#include <mutex>
#endif

#include "androidfw/ResourceTypes.h"
#include "androidfw/TypeWrappers.h"
#include "utils/Atomic.h"
#include "utils/ByteOrder.h"
#include "utils/Debug.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/String16.h"
#include "utils/String8.h"

#ifdef __ANDROID__
#include <binder/TextOutput.h>
#endif

#ifndef INT32_MAX
#define INT32_MAX ((int32_t)(2147483647))
#endif

namespace android {

#if defined(_WIN32)
#undef  nhtol
#undef  htonl
#define ntohl(x)    ( ((x) << 24) | (((x) >> 24) & 255) | (((x) << 8) & 0xff0000) | (((x) >> 8) & 0xff00) )
#define htonl(x)    ntohl(x)
#define ntohs(x)    ( (((x) << 8) & 0xff00) | (((x) >> 8) & 255) )
#define htons(x)    ntohs(x)
#endif

#define IDMAP_MAGIC             0x504D4449
#define IDMAP_CURRENT_VERSION   0x00000001

#define APP_PACKAGE_ID      0x7f
#define SYS_PACKAGE_ID      0x01

static const bool kDebugStringPoolNoisy = false;
static const bool kDebugXMLNoisy = false;
static const bool kDebugTableNoisy = false;
static const bool kDebugTableGetEntry = false;
static const bool kDebugTableSuperNoisy = false;
static const bool kDebugLoadTableNoisy = false;
static const bool kDebugLoadTableSuperNoisy = false;
static const bool kDebugTableTheme = false;
static const bool kDebugResXMLTree = false;
static const bool kDebugLibNoisy = false;

// TODO: This code uses 0xFFFFFFFF converted to bag_set* as a sentinel value. This is bad practice.

// Standard C isspace() is only required to look at the low byte of its input, so
// produces incorrect results for UTF-16 characters.  For safety's sake, assume that
// any high-byte UTF-16 code point is not whitespace.
inline int isspace16(char16_t c) {
    return (c < 0x0080 && isspace(c));
}

template<typename T>
inline static T max(T a, T b) {
    return a > b ? a : b;
}

// range checked; guaranteed to NUL-terminate within the stated number of available slots
// NOTE: if this truncates the dst string due to running out of space, no attempt is
// made to avoid splitting surrogate pairs.
static void strcpy16_dtoh(char16_t* dst, const uint16_t* src, size_t avail)
{
    char16_t* last = dst + avail - 1;
    while (*src && (dst < last)) {
        char16_t s = dtohs(static_cast<char16_t>(*src));
        *dst++ = s;
        src++;
    }
    *dst = 0;
}

static status_t validate_chunk(const ResChunk_header* chunk,
                               size_t minSize,
                               const uint8_t* dataEnd,
                               const char* name)
{
    const uint16_t headerSize = dtohs(chunk->headerSize);
    const uint32_t size = dtohl(chunk->size);

    if (headerSize >= minSize) {
        if (headerSize <= size) {
            if (((headerSize|size)&0x3) == 0) {
                if ((size_t)size <= (size_t)(dataEnd-((const uint8_t*)chunk))) {
                    return NO_ERROR;
                }
                ALOGW("%s data size 0x%x extends beyond resource end %p.",
                     name, size, (void*)(dataEnd-((const uint8_t*)chunk)));
                return BAD_TYPE;
            }
            ALOGW("%s size 0x%x or headerSize 0x%x is not on an integer boundary.",
                 name, (int)size, (int)headerSize);
            return BAD_TYPE;
        }
        ALOGW("%s size 0x%x is smaller than header size 0x%x.",
             name, size, headerSize);
        return BAD_TYPE;
    }
    ALOGW("%s header size 0x%04x is too small.",
         name, headerSize);
    return BAD_TYPE;
}

inline void Res_value::copyFrom_dtoh(const Res_value& src)
{
    size = dtohs(src.size);
    res0 = src.res0;
    dataType = src.dataType;
    data = dtohl(src.data);
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------

ResStringPool::ResStringPool()
    : mError(NO_INIT), mOwnedData(NULL), mHeader(NULL), mCache(NULL)
{
}

ResStringPool::ResStringPool(const void* data, size_t size, bool copyData)
    : mError(NO_INIT), mOwnedData(NULL), mHeader(NULL), mCache(NULL)
{
    setTo(data, size, copyData);
}

ResStringPool::~ResStringPool()
{
    uninit();
}

void ResStringPool::setToEmpty()
{
    uninit();

    mOwnedData = calloc(1, sizeof(ResStringPool_header));
    ResStringPool_header* header = (ResStringPool_header*) mOwnedData;
    mSize = 0;
    mEntries = NULL;
    mStrings = NULL;
    mStringPoolSize = 0;
    mEntryStyles = NULL;
    mStyles = NULL;
    mStylePoolSize = 0;
    mHeader = (const ResStringPool_header*) header;
}

status_t ResStringPool::setTo(const void* data, size_t size, bool copyData)
{
    if (!data || !size) {
        return (mError=BAD_TYPE);
    }

    uninit();

    const bool notDeviceEndian = htods(0xf0) != 0xf0;

    if (copyData || notDeviceEndian) {
        mOwnedData = malloc(size);
        if (mOwnedData == NULL) {
            return (mError=NO_MEMORY);
        }
        memcpy(mOwnedData, data, size);
        data = mOwnedData;
    }

    mHeader = (const ResStringPool_header*)data;

    if (notDeviceEndian) {
        ResStringPool_header* h = const_cast<ResStringPool_header*>(mHeader);
        h->header.headerSize = dtohs(mHeader->header.headerSize);
        h->header.type = dtohs(mHeader->header.type);
        h->header.size = dtohl(mHeader->header.size);
        h->stringCount = dtohl(mHeader->stringCount);
        h->styleCount = dtohl(mHeader->styleCount);
        h->flags = dtohl(mHeader->flags);
        h->stringsStart = dtohl(mHeader->stringsStart);
        h->stylesStart = dtohl(mHeader->stylesStart);
    }

    if (mHeader->header.headerSize > mHeader->header.size
            || mHeader->header.size > size) {
        ALOGW("Bad string block: header size %d or total size %d is larger than data size %d\n",
                (int)mHeader->header.headerSize, (int)mHeader->header.size, (int)size);
        return (mError=BAD_TYPE);
    }
    mSize = mHeader->header.size;
    mEntries = (const uint32_t*)
        (((const uint8_t*)data)+mHeader->header.headerSize);

    if (mHeader->stringCount > 0) {
        if ((mHeader->stringCount*sizeof(uint32_t) < mHeader->stringCount)  // uint32 overflow?
            || (mHeader->header.headerSize+(mHeader->stringCount*sizeof(uint32_t)))
                > size) {
            ALOGW("Bad string block: entry of %d items extends past data size %d\n",
                    (int)(mHeader->header.headerSize+(mHeader->stringCount*sizeof(uint32_t))),
                    (int)size);
            return (mError=BAD_TYPE);
        }

        size_t charSize;
        if (mHeader->flags&ResStringPool_header::UTF8_FLAG) {
            charSize = sizeof(uint8_t);
        } else {
            charSize = sizeof(uint16_t);
        }

        // There should be at least space for the smallest string
        // (2 bytes length, null terminator).
        if (mHeader->stringsStart >= (mSize - sizeof(uint16_t))) {
            ALOGW("Bad string block: string pool starts at %d, after total size %d\n",
                    (int)mHeader->stringsStart, (int)mHeader->header.size);
            return (mError=BAD_TYPE);
        }

        mStrings = (const void*)
            (((const uint8_t*)data) + mHeader->stringsStart);

        if (mHeader->styleCount == 0) {
            mStringPoolSize = (uint32_t) ((mSize - mHeader->stringsStart) / charSize);
        } else {
            // check invariant: styles starts before end of data
            if (mHeader->stylesStart >= (mSize - sizeof(uint16_t))) {
                ALOGW("Bad style block: style block starts at %d past data size of %d\n",
                    (int)mHeader->stylesStart, (int)mHeader->header.size);
                return (mError=BAD_TYPE);
            }
            // check invariant: styles follow the strings
            if (mHeader->stylesStart <= mHeader->stringsStart) {
                ALOGW("Bad style block: style block starts at %d, before strings at %d\n",
                    (int)mHeader->stylesStart, (int)mHeader->stringsStart);
                return (mError=BAD_TYPE);
            }
            mStringPoolSize =
                (mHeader->stylesStart-mHeader->stringsStart)/charSize;
        }

        // check invariant: stringCount > 0 requires a string pool to exist
        if (mStringPoolSize == 0) {
            ALOGW("Bad string block: stringCount is %d but pool size is 0\n", (int)mHeader->stringCount);
            return (mError=BAD_TYPE);
        }

        if (notDeviceEndian) {
            size_t i;
            uint32_t* e = const_cast<uint32_t*>(mEntries);
            for (i=0; i<mHeader->stringCount; i++) {
                e[i] = dtohl(mEntries[i]);
            }
            if (!(mHeader->flags&ResStringPool_header::UTF8_FLAG)) {
                const uint16_t* strings = (const uint16_t*)mStrings;
                uint16_t* s = const_cast<uint16_t*>(strings);
                for (i=0; i<mStringPoolSize; i++) {
                    s[i] = dtohs(strings[i]);
                }
            }
        }

        if ((mHeader->flags & ResStringPool_header::UTF8_FLAG &&
             ((uint8_t*)mStrings)[mStringPoolSize - 1] != 0) ||
            (!(mHeader->flags & ResStringPool_header::UTF8_FLAG) &&
             ((uint16_t*)mStrings)[mStringPoolSize - 1] != 0)) {
          ALOGW("Bad string block: last string is not 0-terminated\n");
          return (mError = BAD_TYPE);
        }
    } else {
        mStrings = NULL;
        mStringPoolSize = 0;
    }

    if (mHeader->styleCount > 0) {
        mEntryStyles = mEntries + mHeader->stringCount;
        // invariant: integer overflow in calculating mEntryStyles
        if (mEntryStyles < mEntries) {
            ALOGW("Bad string block: integer overflow finding styles\n");
            return (mError=BAD_TYPE);
        }

        if (((const uint8_t*)mEntryStyles-(const uint8_t*)mHeader) > (int)size) {
            ALOGW("Bad string block: entry of %d styles extends past data size %d\n",
                    (int)((const uint8_t*)mEntryStyles-(const uint8_t*)mHeader),
                    (int)size);
            return (mError=BAD_TYPE);
        }
        mStyles = (const uint32_t*)
            (((const uint8_t*)data)+mHeader->stylesStart);
        if (mHeader->stylesStart >= mHeader->header.size) {
            ALOGW("Bad string block: style pool starts %d, after total size %d\n",
                    (int)mHeader->stylesStart, (int)mHeader->header.size);
            return (mError=BAD_TYPE);
        }
        mStylePoolSize =
            (mHeader->header.size-mHeader->stylesStart)/sizeof(uint32_t);

        if (notDeviceEndian) {
            size_t i;
            uint32_t* e = const_cast<uint32_t*>(mEntryStyles);
            for (i=0; i<mHeader->styleCount; i++) {
                e[i] = dtohl(mEntryStyles[i]);
            }
            uint32_t* s = const_cast<uint32_t*>(mStyles);
            for (i=0; i<mStylePoolSize; i++) {
                s[i] = dtohl(mStyles[i]);
            }
        }

        const ResStringPool_span endSpan = {
            { htodl(ResStringPool_span::END) },
            htodl(ResStringPool_span::END), htodl(ResStringPool_span::END)
        };
        if (memcmp(&mStyles[mStylePoolSize-(sizeof(endSpan)/sizeof(uint32_t))],
                   &endSpan, sizeof(endSpan)) != 0) {
            ALOGW("Bad string block: last style is not 0xFFFFFFFF-terminated\n");
            return (mError=BAD_TYPE);
        }
    } else {
        mEntryStyles = NULL;
        mStyles = NULL;
        mStylePoolSize = 0;
    }

    return (mError=NO_ERROR);
}

status_t ResStringPool::getError() const
{
    return mError;
}

void ResStringPool::uninit()
{
    mError = NO_INIT;
    if (mHeader != NULL && mCache != NULL) {
        for (size_t x = 0; x < mHeader->stringCount; x++) {
            if (mCache[x] != NULL) {
                free(mCache[x]);
                mCache[x] = NULL;
            }
        }
        free(mCache);
        mCache = NULL;
    }
    if (mOwnedData) {
        free(mOwnedData);
        mOwnedData = NULL;
    }
}

/**
 * Strings in UTF-16 format have length indicated by a length encoded in the
 * stored data. It is either 1 or 2 characters of length data. This allows a
 * maximum length of 0x7FFFFFF (2147483647 bytes), but if you're storing that
 * much data in a string, you're abusing them.
 *
 * If the high bit is set, then there are two characters or 4 bytes of length
 * data encoded. In that case, drop the high bit of the first character and
 * add it together with the next character.
 */
static inline size_t
decodeLength(const uint16_t** str)
{
    size_t len = **str;
    if ((len & 0x8000) != 0) {
        (*str)++;
        len = ((len & 0x7FFF) << 16) | **str;
    }
    (*str)++;
    return len;
}

/**
 * Strings in UTF-8 format have length indicated by a length encoded in the
 * stored data. It is either 1 or 2 characters of length data. This allows a
 * maximum length of 0x7FFF (32767 bytes), but you should consider storing
 * text in another way if you're using that much data in a single string.
 *
 * If the high bit is set, then there are two characters or 2 bytes of length
 * data encoded. In that case, drop the high bit of the first character and
 * add it together with the next character.
 */
static inline size_t
decodeLength(const uint8_t** str)
{
    size_t len = **str;
    if ((len & 0x80) != 0) {
        (*str)++;
        len = ((len & 0x7F) << 8) | **str;
    }
    (*str)++;
    return len;
}

const char16_t* ResStringPool::stringAt(size_t idx, size_t* u16len) const
{
    if (mError == NO_ERROR && idx < mHeader->stringCount) {
        const bool isUTF8 = (mHeader->flags&ResStringPool_header::UTF8_FLAG) != 0;
        const uint32_t off = mEntries[idx]/(isUTF8?sizeof(uint8_t):sizeof(uint16_t));
        if (off < (mStringPoolSize-1)) {
            if (!isUTF8) {
                const uint16_t* strings = (uint16_t*)mStrings;
                const uint16_t* str = strings+off;

                *u16len = decodeLength(&str);
                if ((uint32_t)(str+*u16len-strings) < mStringPoolSize) {
                    return reinterpret_cast<const char16_t*>(str);
                } else {
                    ALOGW("Bad string block: string #%d extends to %d, past end at %d\n",
                            (int)idx, (int)(str+*u16len-strings), (int)mStringPoolSize);
                }
            } else {
                const uint8_t* strings = (uint8_t*)mStrings;
                const uint8_t* u8str = strings+off;

                *u16len = decodeLength(&u8str);
                size_t u8len = decodeLength(&u8str);

                // encLen must be less than 0x7FFF due to encoding.
                if ((uint32_t)(u8str+u8len-strings) < mStringPoolSize) {
#if !defined(_MSC_VER) && !defined(__MINGW64__) && !defined(__MINGW32__)
                    AutoMutex lock(mDecodeLock);
#else
                    std::lock_guard<std::mutex> lock(mDecodeLock);
#endif

                    if (mCache == NULL) {
#ifndef __ANDROID__
                        if (kDebugStringPoolNoisy) {
                            ALOGI("CREATING STRING CACHE OF %zu bytes",
                                    mHeader->stringCount*sizeof(char16_t**));
                        }
#else
                        // We do not want to be in this case when actually running Android.
                        ALOGW("CREATING STRING CACHE OF %zu bytes",
                                static_cast<size_t>(mHeader->stringCount*sizeof(char16_t**)));
#endif
                        mCache = (char16_t**)calloc(mHeader->stringCount, sizeof(char16_t**));
                        if (mCache == NULL) {
                            ALOGW("No memory trying to allocate decode cache table of %d bytes\n",
                                    (int)(mHeader->stringCount*sizeof(char16_t**)));
                            return NULL;
                        }
                    }

                    if (mCache[idx] != NULL) {
                        return mCache[idx];
                    }

                    ssize_t actualLen = utf8_to_utf16_length(u8str, u8len);
                    if (actualLen < 0 || (size_t)actualLen != *u16len) {
                        ALOGW("Bad string block: string #%lld decoded length is not correct "
                                "%lld vs %llu\n",
                                (long long)idx, (long long)actualLen, (long long)*u16len);
                        return NULL;
                    }

                    char16_t *u16str = (char16_t *)calloc(*u16len+1, sizeof(char16_t));
                    if (!u16str) {
                        ALOGW("No memory when trying to allocate decode cache for string #%d\n",
                                (int)idx);
                        return NULL;
                    }

                    if (kDebugStringPoolNoisy) {
                        ALOGI("Caching UTF8 string: %s", u8str);
                    }
                    utf8_to_utf16(u8str, u8len, u16str);
                    mCache[idx] = u16str;
                    return u16str;
                } else {
                    ALOGW("Bad string block: string #%lld extends to %lld, past end at %lld\n",
                            (long long)idx, (long long)(u8str+u8len-strings),
                            (long long)mStringPoolSize);
                }
            }
        } else {
            ALOGW("Bad string block: string #%d entry is at %d, past end at %d\n",
                    (int)idx, (int)(off*sizeof(uint16_t)),
                    (int)(mStringPoolSize*sizeof(uint16_t)));
        }
    }
    return NULL;
}

const char* ResStringPool::string8At(size_t idx, size_t* outLen) const
{
    if (mError == NO_ERROR && idx < mHeader->stringCount) {
        if ((mHeader->flags&ResStringPool_header::UTF8_FLAG) == 0) {
            return NULL;
        }
        const uint32_t off = mEntries[idx]/sizeof(char);
        if (off < (mStringPoolSize-1)) {
            const uint8_t* strings = (uint8_t*)mStrings;
            const uint8_t* str = strings+off;
            *outLen = decodeLength(&str);
            size_t encLen = decodeLength(&str);
            if ((uint32_t)(str+encLen-strings) < mStringPoolSize) {
                return (const char*)str;
            } else {
                ALOGW("Bad string block: string #%d extends to %d, past end at %d\n",
                        (int)idx, (int)(str+encLen-strings), (int)mStringPoolSize);
            }
        } else {
            ALOGW("Bad string block: string #%d entry is at %d, past end at %d\n",
                    (int)idx, (int)(off*sizeof(uint16_t)),
                    (int)(mStringPoolSize*sizeof(uint16_t)));
        }
    }
    return NULL;
}

const String8 ResStringPool::string8ObjectAt(size_t idx) const
{
    size_t len;
    const char *str = string8At(idx, &len);
    if (str != NULL) {
        return String8(str, len);
    }

    const char16_t *str16 = stringAt(idx, &len);
    if (str16 != NULL) {
        return String8(str16, len);
    }
    return String8();
}

const ResStringPool_span* ResStringPool::styleAt(const ResStringPool_ref& ref) const
{
    return styleAt(ref.index);
}

const ResStringPool_span* ResStringPool::styleAt(size_t idx) const
{
    if (mError == NO_ERROR && idx < mHeader->styleCount) {
        const uint32_t off = (mEntryStyles[idx]/sizeof(uint32_t));
        if (off < mStylePoolSize) {
            return (const ResStringPool_span*)(mStyles+off);
        } else {
            ALOGW("Bad string block: style #%d entry is at %d, past end at %d\n",
                    (int)idx, (int)(off*sizeof(uint32_t)),
                    (int)(mStylePoolSize*sizeof(uint32_t)));
        }
    }
    return NULL;
}

ssize_t ResStringPool::indexOfString(const char16_t* str, size_t strLen) const
{
    if (mError != NO_ERROR) {
        return mError;
    }

    size_t len;

    if ((mHeader->flags&ResStringPool_header::UTF8_FLAG) != 0) {
        if (kDebugStringPoolNoisy) {
            ALOGI("indexOfString UTF-8: %s", String8(str, strLen).string());
        }

        // The string pool contains UTF 8 strings; we don't want to cause
        // temporary UTF-16 strings to be created as we search.
        if (mHeader->flags&ResStringPool_header::SORTED_FLAG) {
            // Do a binary search for the string...  this is a little tricky,
            // because the strings are sorted with strzcmp16().  So to match
            // the ordering, we need to convert strings in the pool to UTF-16.
            // But we don't want to hit the cache, so instead we will have a
            // local temporary allocation for the conversions.
            char16_t* convBuffer = (char16_t*)malloc(strLen+4);
            ssize_t l = 0;
            ssize_t h = mHeader->stringCount-1;

            ssize_t mid;
            while (l <= h) {
                mid = l + (h - l)/2;
                const uint8_t* s = (const uint8_t*)string8At(mid, &len);
                int c;
                if (s != NULL) {
                    char16_t* end = utf8_to_utf16_n(s, len, convBuffer, strLen+3);
                    *end = 0;
                    c = strzcmp16(convBuffer, end-convBuffer, str, strLen);
                } else {
                    c = -1;
                }
                if (kDebugStringPoolNoisy) {
                    ALOGI("Looking at %s, cmp=%d, l/mid/h=%d/%d/%d\n",
                            (const char*)s, c, (int)l, (int)mid, (int)h);
                }
                if (c == 0) {
                    if (kDebugStringPoolNoisy) {
                        ALOGI("MATCH!");
                    }
                    free(convBuffer);
                    return mid;
                } else if (c < 0) {
                    l = mid + 1;
                } else {
                    h = mid - 1;
                }
            }
            free(convBuffer);
        } else {
            // It is unusual to get the ID from an unsorted string block...
            // most often this happens because we want to get IDs for style
            // span tags; since those always appear at the end of the string
            // block, start searching at the back.
            String8 str8(str, strLen);
            const size_t str8Len = str8.size();
            for (int i=mHeader->stringCount-1; i>=0; i--) {
                const char* s = string8At(i, &len);
                if (kDebugStringPoolNoisy) {
                    ALOGI("Looking at %s, i=%d\n", String8(s).string(), i);
                }
                if (s && str8Len == len && memcmp(s, str8.string(), str8Len) == 0) {
                    if (kDebugStringPoolNoisy) {
                        ALOGI("MATCH!");
                    }
                    return i;
                }
            }
        }

    } else {
        if (kDebugStringPoolNoisy) {
            ALOGI("indexOfString UTF-16: %s", String8(str, strLen).string());
        }

        if (mHeader->flags&ResStringPool_header::SORTED_FLAG) {
            // Do a binary search for the string...
            ssize_t l = 0;
            ssize_t h = mHeader->stringCount-1;

            ssize_t mid;
            while (l <= h) {
                mid = l + (h - l)/2;
                const char16_t* s = stringAt(mid, &len);
                int c = s ? strzcmp16(s, len, str, strLen) : -1;
                if (kDebugStringPoolNoisy) {
                    ALOGI("Looking at %s, cmp=%d, l/mid/h=%d/%d/%d\n",
                            String8(s).string(), c, (int)l, (int)mid, (int)h);
                }
                if (c == 0) {
                    if (kDebugStringPoolNoisy) {
                        ALOGI("MATCH!");
                    }
                    return mid;
                } else if (c < 0) {
                    l = mid + 1;
                } else {
                    h = mid - 1;
                }
            }
        } else {
            // It is unusual to get the ID from an unsorted string block...
            // most often this happens because we want to get IDs for style
            // span tags; since those always appear at the end of the string
            // block, start searching at the back.
            for (int i=mHeader->stringCount-1; i>=0; i--) {
                const char16_t* s = stringAt(i, &len);
                if (kDebugStringPoolNoisy) {
                    ALOGI("Looking at %s, i=%d\n", String8(s).string(), i);
                }
                if (s && strLen == len && strzcmp16(s, len, str, strLen) == 0) {
                    if (kDebugStringPoolNoisy) {
                        ALOGI("MATCH!");
                    }
                    return i;
                }
            }
        }
    }

    return NAME_NOT_FOUND;
}

size_t ResStringPool::size() const
{
    return (mError == NO_ERROR) ? mHeader->stringCount : 0;
}

size_t ResStringPool::styleCount() const
{
    return (mError == NO_ERROR) ? mHeader->styleCount : 0;
}

size_t ResStringPool::bytes() const
{
    return (mError == NO_ERROR) ? mHeader->header.size : 0;
}

bool ResStringPool::isSorted() const
{
    return (mHeader->flags&ResStringPool_header::SORTED_FLAG)!=0;
}

bool ResStringPool::isUTF8() const
{
    return (mHeader->flags&ResStringPool_header::UTF8_FLAG)!=0;
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------

ResXMLParser::ResXMLParser(const ResXMLTree& tree)
    : mTree(tree), mEventCode(BAD_DOCUMENT)
{
}

void ResXMLParser::restart()
{
    mCurNode = NULL;
    mEventCode = mTree.mError == NO_ERROR ? START_DOCUMENT : BAD_DOCUMENT;
}
const ResStringPool& ResXMLParser::getStrings() const
{
    return mTree.mStrings;
}

ResXMLParser::event_code_t ResXMLParser::getEventType() const
{
    return mEventCode;
}

ResXMLParser::event_code_t ResXMLParser::next()
{
    if (mEventCode == START_DOCUMENT) {
        mCurNode = mTree.mRootNode;
        mCurExt = mTree.mRootExt;
        return (mEventCode=mTree.mRootCode);
    } else if (mEventCode >= FIRST_CHUNK_CODE) {
        return nextNode();
    }
    return mEventCode;
}

int32_t ResXMLParser::getCommentID() const
{
    return mCurNode != NULL ? dtohl(mCurNode->comment.index) : -1;
}

const char16_t* ResXMLParser::getComment(size_t* outLen) const
{
    int32_t id = getCommentID();
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

uint32_t ResXMLParser::getLineNumber() const
{
    return mCurNode != NULL ? dtohl(mCurNode->lineNumber) : -1;
}

int32_t ResXMLParser::getTextID() const
{
    if (mEventCode == TEXT) {
        return dtohl(((const ResXMLTree_cdataExt*)mCurExt)->data.index);
    }
    return -1;
}

const char16_t* ResXMLParser::getText(size_t* outLen) const
{
    int32_t id = getTextID();
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

ssize_t ResXMLParser::getTextValue(Res_value* outValue) const
{
    if (mEventCode == TEXT) {
        outValue->copyFrom_dtoh(((const ResXMLTree_cdataExt*)mCurExt)->typedData);
        return sizeof(Res_value);
    }
    return BAD_TYPE;
}

int32_t ResXMLParser::getNamespacePrefixID() const
{
    if (mEventCode == START_NAMESPACE || mEventCode == END_NAMESPACE) {
        return dtohl(((const ResXMLTree_namespaceExt*)mCurExt)->prefix.index);
    }
    return -1;
}

const char16_t* ResXMLParser::getNamespacePrefix(size_t* outLen) const
{
    int32_t id = getNamespacePrefixID();
    //printf("prefix=%d  event=%p\n", id, mEventCode);
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

int32_t ResXMLParser::getNamespaceUriID() const
{
    if (mEventCode == START_NAMESPACE || mEventCode == END_NAMESPACE) {
        return dtohl(((const ResXMLTree_namespaceExt*)mCurExt)->uri.index);
    }
    return -1;
}

const char16_t* ResXMLParser::getNamespaceUri(size_t* outLen) const
{
    int32_t id = getNamespaceUriID();
    //printf("uri=%d  event=%p\n", id, mEventCode);
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

int32_t ResXMLParser::getElementNamespaceID() const
{
    if (mEventCode == START_TAG) {
        return dtohl(((const ResXMLTree_attrExt*)mCurExt)->ns.index);
    }
    if (mEventCode == END_TAG) {
        return dtohl(((const ResXMLTree_endElementExt*)mCurExt)->ns.index);
    }
    return -1;
}

const char16_t* ResXMLParser::getElementNamespace(size_t* outLen) const
{
    int32_t id = getElementNamespaceID();
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

int32_t ResXMLParser::getElementNameID() const
{
    if (mEventCode == START_TAG) {
        return dtohl(((const ResXMLTree_attrExt*)mCurExt)->name.index);
    }
    if (mEventCode == END_TAG) {
        return dtohl(((const ResXMLTree_endElementExt*)mCurExt)->name.index);
    }
    return -1;
}

const char16_t* ResXMLParser::getElementName(size_t* outLen) const
{
    int32_t id = getElementNameID();
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

size_t ResXMLParser::getAttributeCount() const
{
    if (mEventCode == START_TAG) {
        return dtohs(((const ResXMLTree_attrExt*)mCurExt)->attributeCount);
    }
    return 0;
}

int32_t ResXMLParser::getAttributeNamespaceID(size_t idx) const
{
    if (mEventCode == START_TAG) {
        const ResXMLTree_attrExt* tag = (const ResXMLTree_attrExt*)mCurExt;
        if (idx < dtohs(tag->attributeCount)) {
            const ResXMLTree_attribute* attr = (const ResXMLTree_attribute*)
                (((const uint8_t*)tag)
                 + dtohs(tag->attributeStart)
                 + (dtohs(tag->attributeSize)*idx));
            return dtohl(attr->ns.index);
        }
    }
    return -2;
}

const char16_t* ResXMLParser::getAttributeNamespace(size_t idx, size_t* outLen) const
{
    int32_t id = getAttributeNamespaceID(idx);
    //printf("attribute namespace=%d  idx=%d  event=%p\n", id, idx, mEventCode);
    if (kDebugXMLNoisy) {
        printf("getAttributeNamespace 0x%zx=0x%x\n", idx, id);
    }
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

const char* ResXMLParser::getAttributeNamespace8(size_t idx, size_t* outLen) const
{
    int32_t id = getAttributeNamespaceID(idx);
    //printf("attribute namespace=%d  idx=%d  event=%p\n", id, idx, mEventCode);
    if (kDebugXMLNoisy) {
        printf("getAttributeNamespace 0x%zx=0x%x\n", idx, id);
    }
    return id >= 0 ? mTree.mStrings.string8At(id, outLen) : NULL;
}

int32_t ResXMLParser::getAttributeNameID(size_t idx) const
{
    if (mEventCode == START_TAG) {
        const ResXMLTree_attrExt* tag = (const ResXMLTree_attrExt*)mCurExt;
        if (idx < dtohs(tag->attributeCount)) {
            const ResXMLTree_attribute* attr = (const ResXMLTree_attribute*)
                (((const uint8_t*)tag)
                 + dtohs(tag->attributeStart)
                 + (dtohs(tag->attributeSize)*idx));
            return dtohl(attr->name.index);
        }
    }
    return -1;
}

const char16_t* ResXMLParser::getAttributeName(size_t idx, size_t* outLen) const
{
    int32_t id = getAttributeNameID(idx);
    //printf("attribute name=%d  idx=%d  event=%p\n", id, idx, mEventCode);
    if (kDebugXMLNoisy) {
        printf("getAttributeName 0x%zx=0x%x\n", idx, id);
    }
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

const char* ResXMLParser::getAttributeName8(size_t idx, size_t* outLen) const
{
    int32_t id = getAttributeNameID(idx);
    //printf("attribute name=%d  idx=%d  event=%p\n", id, idx, mEventCode);
    if (kDebugXMLNoisy) {
        printf("getAttributeName 0x%zx=0x%x\n", idx, id);
    }
    return id >= 0 ? mTree.mStrings.string8At(id, outLen) : NULL;
}

uint32_t ResXMLParser::getAttributeNameResID(size_t idx) const
{
    int32_t id = getAttributeNameID(idx);
    if (id >= 0 && (size_t)id < mTree.mNumResIds) {
        uint32_t resId = dtohl(mTree.mResIds[id]);
        if (mTree.mDynamicRefTable != NULL) {
            mTree.mDynamicRefTable->lookupResourceId(&resId);
        }
        return resId;
    }
    return 0;
}

int32_t ResXMLParser::getAttributeValueStringID(size_t idx) const
{
    if (mEventCode == START_TAG) {
        const ResXMLTree_attrExt* tag = (const ResXMLTree_attrExt*)mCurExt;
        if (idx < dtohs(tag->attributeCount)) {
            const ResXMLTree_attribute* attr = (const ResXMLTree_attribute*)
                (((const uint8_t*)tag)
                 + dtohs(tag->attributeStart)
                 + (dtohs(tag->attributeSize)*idx));
            return dtohl(attr->rawValue.index);
        }
    }
    return -1;
}

const char16_t* ResXMLParser::getAttributeStringValue(size_t idx, size_t* outLen) const
{
    int32_t id = getAttributeValueStringID(idx);
    if (kDebugXMLNoisy) {
        printf("getAttributeValue 0x%zx=0x%x\n", idx, id);
    }
    return id >= 0 ? mTree.mStrings.stringAt(id, outLen) : NULL;
}

int32_t ResXMLParser::getAttributeDataType(size_t idx) const
{
    if (mEventCode == START_TAG) {
        const ResXMLTree_attrExt* tag = (const ResXMLTree_attrExt*)mCurExt;
        if (idx < dtohs(tag->attributeCount)) {
            const ResXMLTree_attribute* attr = (const ResXMLTree_attribute*)
                (((const uint8_t*)tag)
                 + dtohs(tag->attributeStart)
                 + (dtohs(tag->attributeSize)*idx));
            uint8_t type = attr->typedValue.dataType;
            if (type != Res_value::TYPE_DYNAMIC_REFERENCE) {
                return type;
            }

            // This is a dynamic reference. We adjust those references
            // to regular references at this level, so lie to the caller.
            return Res_value::TYPE_REFERENCE;
        }
    }
    return Res_value::TYPE_NULL;
}

int32_t ResXMLParser::getAttributeData(size_t idx) const
{
    if (mEventCode == START_TAG) {
        const ResXMLTree_attrExt* tag = (const ResXMLTree_attrExt*)mCurExt;
        if (idx < dtohs(tag->attributeCount)) {
            const ResXMLTree_attribute* attr = (const ResXMLTree_attribute*)
                (((const uint8_t*)tag)
                 + dtohs(tag->attributeStart)
                 + (dtohs(tag->attributeSize)*idx));
            if (attr->typedValue.dataType != Res_value::TYPE_DYNAMIC_REFERENCE ||
                    mTree.mDynamicRefTable == NULL) {
                return dtohl(attr->typedValue.data);
            }

            uint32_t data = dtohl(attr->typedValue.data);
            if (mTree.mDynamicRefTable->lookupResourceId(&data) == NO_ERROR) {
                return data;
            }
        }
    }
    return 0;
}

ResXMLTree_attribute* ResXMLParser::getAttributePointer(size_t idx) const
{
    if (mEventCode == START_TAG) {
        const ResXMLTree_attrExt* tag = (const ResXMLTree_attrExt*)mCurExt;
        if (idx < dtohs(tag->attributeCount)) {
            return (ResXMLTree_attribute*)
                (((const uint8_t*)tag)
                 + dtohs(tag->attributeStart)
                 + (dtohs(tag->attributeSize)*idx));
        }
    }

    return nullptr;
}

ssize_t ResXMLParser::getAttributeValue(size_t idx, Res_value* outValue) const
{
    const ResXMLTree_attribute* attr = getAttributePointer(idx);
    if (attr == nullptr) {
        return BAD_TYPE;
    }

    outValue->copyFrom_dtoh(attr->typedValue);
    if (mTree.mDynamicRefTable != NULL &&
            mTree.mDynamicRefTable->lookupResourceValue(outValue) != NO_ERROR) {
        return BAD_TYPE;
    }
    return sizeof(Res_value);
}

ssize_t ResXMLParser::indexOfAttribute(const char* ns, const char* attr) const
{
    String16 nsStr(ns != NULL ? ns : "");
    String16 attrStr(attr);
    return indexOfAttribute(ns ? nsStr.string() : NULL, ns ? nsStr.size() : 0,
                            attrStr.string(), attrStr.size());
}

ssize_t ResXMLParser::indexOfAttribute(const char16_t* ns, size_t nsLen,
                                       const char16_t* attr, size_t attrLen) const
{
    if (mEventCode == START_TAG) {
        if (attr == NULL) {
            return NAME_NOT_FOUND;
        }
        const size_t N = getAttributeCount();
        if (mTree.mStrings.isUTF8()) {
            String8 ns8, attr8;
            if (ns != NULL) {
                ns8 = String8(ns, nsLen);
            }
            attr8 = String8(attr, attrLen);
            if (kDebugStringPoolNoisy) {
                ALOGI("indexOfAttribute UTF8 %s (%zu) / %s (%zu)", ns8.string(), nsLen,
                        attr8.string(), attrLen);
            }
            for (size_t i=0; i<N; i++) {
                size_t curNsLen = 0, curAttrLen = 0;
                const char* curNs = getAttributeNamespace8(i, &curNsLen);
                const char* curAttr = getAttributeName8(i, &curAttrLen);
                if (kDebugStringPoolNoisy) {
                    ALOGI("  curNs=%s (%zu), curAttr=%s (%zu)", curNs, curNsLen, curAttr, curAttrLen);
                }
                if (curAttr != NULL && curNsLen == nsLen && curAttrLen == attrLen
                        && memcmp(attr8.string(), curAttr, attrLen) == 0) {
                    if (ns == NULL) {
                        if (curNs == NULL) {
                            if (kDebugStringPoolNoisy) {
                                ALOGI("  FOUND!");
                            }
                            return i;
                        }
                    } else if (curNs != NULL) {
                        //printf(" --> ns=%s, curNs=%s\n",
                        //       String8(ns).string(), String8(curNs).string());
                        if (memcmp(ns8.string(), curNs, nsLen) == 0) {
                            if (kDebugStringPoolNoisy) {
                                ALOGI("  FOUND!");
                            }
                            return i;
                        }
                    }
                }
            }
        } else {
            if (kDebugStringPoolNoisy) {
                ALOGI("indexOfAttribute UTF16 %s (%zu) / %s (%zu)",
                        String8(ns, nsLen).string(), nsLen,
                        String8(attr, attrLen).string(), attrLen);
            }
            for (size_t i=0; i<N; i++) {
                size_t curNsLen = 0, curAttrLen = 0;
                const char16_t* curNs = getAttributeNamespace(i, &curNsLen);
                const char16_t* curAttr = getAttributeName(i, &curAttrLen);
                if (kDebugStringPoolNoisy) {
                    ALOGI("  curNs=%s (%zu), curAttr=%s (%zu)",
                            String8(curNs, curNsLen).string(), curNsLen,
                            String8(curAttr, curAttrLen).string(), curAttrLen);
                }
                if (curAttr != NULL && curNsLen == nsLen && curAttrLen == attrLen
                        && (memcmp(attr, curAttr, attrLen*sizeof(char16_t)) == 0)) {
                    if (ns == NULL) {
                        if (curNs == NULL) {
                            if (kDebugStringPoolNoisy) {
                                ALOGI("  FOUND!");
                            }
                            return i;
                        }
                    } else if (curNs != NULL) {
                        //printf(" --> ns=%s, curNs=%s\n",
                        //       String8(ns).string(), String8(curNs).string());
                        if (memcmp(ns, curNs, nsLen*sizeof(char16_t)) == 0) {
                            if (kDebugStringPoolNoisy) {
                                ALOGI("  FOUND!");
                            }
                            return i;
                        }
                    }
                }
            }
        }
    }

    return NAME_NOT_FOUND;
}

ssize_t ResXMLParser::indexOfID() const
{
    if (mEventCode == START_TAG) {
        const ssize_t idx = dtohs(((const ResXMLTree_attrExt*)mCurExt)->idIndex);
        if (idx > 0) return (idx-1);
    }
    return NAME_NOT_FOUND;
}

ssize_t ResXMLParser::indexOfClass() const
{
    if (mEventCode == START_TAG) {
        const ssize_t idx = dtohs(((const ResXMLTree_attrExt*)mCurExt)->classIndex);
        if (idx > 0) return (idx-1);
    }
    return NAME_NOT_FOUND;
}

ssize_t ResXMLParser::indexOfStyle() const
{
    if (mEventCode == START_TAG) {
        const ssize_t idx = dtohs(((const ResXMLTree_attrExt*)mCurExt)->styleIndex);
        if (idx > 0) return (idx-1);
    }
    return NAME_NOT_FOUND;
}

ResXMLParser::event_code_t ResXMLParser::nextNode()
{
    if (mEventCode < 0) {
        return mEventCode;
    }

    do {
        const ResXMLTree_node* next = (const ResXMLTree_node*)
            (((const uint8_t*)mCurNode) + dtohl(mCurNode->header.size));
        if (kDebugXMLNoisy) {
            ALOGI("Next node: prev=%p, next=%p\n", mCurNode, next);
        }

        if (((const uint8_t*)next) >= mTree.mDataEnd) {
            mCurNode = NULL;
            return (mEventCode=END_DOCUMENT);
        }

        if (mTree.validateNode(next) != NO_ERROR) {
            mCurNode = NULL;
            return (mEventCode=BAD_DOCUMENT);
        }

        mCurNode = next;
        const uint16_t headerSize = dtohs(next->header.headerSize);
        const uint32_t totalSize = dtohl(next->header.size);
        mCurExt = ((const uint8_t*)next) + headerSize;
        size_t minExtSize = 0;
        event_code_t eventCode = (event_code_t)dtohs(next->header.type);
        switch ((mEventCode=eventCode)) {
            case RES_XML_START_NAMESPACE_TYPE:
            case RES_XML_END_NAMESPACE_TYPE:
                minExtSize = sizeof(ResXMLTree_namespaceExt);
                break;
            case RES_XML_START_ELEMENT_TYPE:
                minExtSize = sizeof(ResXMLTree_attrExt);
                break;
            case RES_XML_END_ELEMENT_TYPE:
                minExtSize = sizeof(ResXMLTree_endElementExt);
                break;
            case RES_XML_CDATA_TYPE:
                minExtSize = sizeof(ResXMLTree_cdataExt);
                break;
            default:
                ALOGW("Unknown XML block: header type %d in node at %d\n",
                     (int)dtohs(next->header.type),
                     (int)(((const uint8_t*)next)-((const uint8_t*)mTree.mHeader)));
                continue;
        }

        if ((totalSize-headerSize) < minExtSize) {
            ALOGW("Bad XML block: header type 0x%x in node at 0x%x has size %d, need %d\n",
                 (int)dtohs(next->header.type),
                 (int)(((const uint8_t*)next)-((const uint8_t*)mTree.mHeader)),
                 (int)(totalSize-headerSize), (int)minExtSize);
            return (mEventCode=BAD_DOCUMENT);
        }

        //printf("CurNode=%p, CurExt=%p, headerSize=%d, minExtSize=%d\n",
        //       mCurNode, mCurExt, headerSize, minExtSize);

        return eventCode;
    } while (true);
}

void ResXMLParser::getPosition(ResXMLParser::ResXMLPosition* pos) const
{
    pos->eventCode = mEventCode;
    pos->curNode = mCurNode;
    pos->curExt = mCurExt;
}

void ResXMLParser::setPosition(const ResXMLParser::ResXMLPosition& pos)
{
    mEventCode = pos.eventCode;
    mCurNode = pos.curNode;
    mCurExt = pos.curExt;
}

// --------------------------------------------------------------------

static volatile int32_t gCount = 0;

ResXMLTree::ResXMLTree(const DynamicRefTable* dynamicRefTable)
    : ResXMLParser(*this)
    , mDynamicRefTable(dynamicRefTable)
    , mError(NO_INIT), mOwnedData(NULL)
{
    if (kDebugResXMLTree) {
        ALOGI("Creating ResXMLTree %p #%d\n", this, android_atomic_inc(&gCount)+1);
    }
    restart();
}

ResXMLTree::ResXMLTree()
    : ResXMLParser(*this)
    , mDynamicRefTable(NULL)
    , mError(NO_INIT), mOwnedData(NULL)
{
    if (kDebugResXMLTree) {
        ALOGI("Creating ResXMLTree %p #%d\n", this, android_atomic_inc(&gCount)+1);
    }
    restart();
}

ResXMLTree::~ResXMLTree()
{
    if (kDebugResXMLTree) {
        ALOGI("Destroying ResXMLTree in %p #%d\n", this, android_atomic_dec(&gCount)-1);
    }
    uninit();
}

status_t ResXMLTree::setTo(const void* data, size_t size, bool copyData)
{
    uninit();
    mEventCode = START_DOCUMENT;

    if (!data || !size) {
        return (mError=BAD_TYPE);
    }

    if (copyData) {
        mOwnedData = malloc(size);
        if (mOwnedData == NULL) {
            return (mError=NO_MEMORY);
        }
        memcpy(mOwnedData, data, size);
        data = mOwnedData;
    }

    mHeader = (const ResXMLTree_header*)data;
    mSize = dtohl(mHeader->header.size);
    if (dtohs(mHeader->header.headerSize) > mSize || mSize > size) {
        ALOGW("Bad XML block: header size %d or total size %d is larger than data size %d\n",
             (int)dtohs(mHeader->header.headerSize),
             (int)dtohl(mHeader->header.size), (int)size);
        mError = BAD_TYPE;
        restart();
        return mError;
    }
    mDataEnd = ((const uint8_t*)mHeader) + mSize;

    mStrings.uninit();
    mRootNode = NULL;
    mResIds = NULL;
    mNumResIds = 0;

    // First look for a couple interesting chunks: the string block
    // and first XML node.
    const ResChunk_header* chunk =
        (const ResChunk_header*)(((const uint8_t*)mHeader) + dtohs(mHeader->header.headerSize));
    const ResChunk_header* lastChunk = chunk;
    while (((const uint8_t*)chunk) < (mDataEnd-sizeof(ResChunk_header)) &&
           ((const uint8_t*)chunk) < (mDataEnd-dtohl(chunk->size))) {
        status_t err = validate_chunk(chunk, sizeof(ResChunk_header), mDataEnd, "XML");
        if (err != NO_ERROR) {
            mError = err;
            goto done;
        }
        const uint16_t type = dtohs(chunk->type);
        const size_t size = dtohl(chunk->size);
        if (kDebugXMLNoisy) {
            printf("Scanning @ %p: type=0x%x, size=0x%zx\n",
                    (void*)(((uintptr_t)chunk)-((uintptr_t)mHeader)), type, size);
        }
        if (type == RES_STRING_POOL_TYPE) {
            mStrings.setTo(chunk, size);
        } else if (type == RES_XML_RESOURCE_MAP_TYPE) {
            mResIds = (uint32_t*)
                (((const uint8_t*)chunk)+dtohs(chunk->headerSize));
            mNumResIds = (dtohl(chunk->size)-dtohs(chunk->headerSize))/sizeof(uint32_t);
        } else if (type >= RES_XML_FIRST_CHUNK_TYPE
                   && type <= RES_XML_LAST_CHUNK_TYPE) {
            if (validateNode((const ResXMLTree_node*)chunk) != NO_ERROR) {
                mError = BAD_TYPE;
                goto done;
            }
            mCurNode = (const ResXMLTree_node*)lastChunk;
            if (nextNode() == BAD_DOCUMENT) {
                mError = BAD_TYPE;
                goto done;
            }
            mRootNode = mCurNode;
            mRootExt = mCurExt;
            mRootCode = mEventCode;
            break;
        } else {
            if (kDebugXMLNoisy) {
                printf("Skipping unknown chunk!\n");
            }
        }
        lastChunk = chunk;
        chunk = (const ResChunk_header*)
            (((const uint8_t*)chunk) + size);
    }

    if (mRootNode == NULL) {
        ALOGW("Bad XML block: no root element node found\n");
        mError = BAD_TYPE;
        goto done;
    }

    mError = mStrings.getError();

done:
    restart();
    return mError;
}

status_t ResXMLTree::getError() const
{
    return mError;
}

void ResXMLTree::uninit()
{
    mError = NO_INIT;
    mStrings.uninit();
    if (mOwnedData) {
        free(mOwnedData);
        mOwnedData = NULL;
    }
    restart();
}

status_t ResXMLTree::validateNode(const ResXMLTree_node* node) const
{
    const uint16_t eventCode = dtohs(node->header.type);

    status_t err = validate_chunk(
        &node->header, sizeof(ResXMLTree_node),
        mDataEnd, "ResXMLTree_node");

    if (err >= NO_ERROR) {
        // Only perform additional validation on START nodes
        if (eventCode != RES_XML_START_ELEMENT_TYPE) {
            return NO_ERROR;
        }

        const uint16_t headerSize = dtohs(node->header.headerSize);
        const uint32_t size = dtohl(node->header.size);
        const ResXMLTree_attrExt* attrExt = (const ResXMLTree_attrExt*)
            (((const uint8_t*)node) + headerSize);
        // check for sensical values pulled out of the stream so far...
        if ((size >= headerSize + sizeof(ResXMLTree_attrExt))
                && ((void*)attrExt > (void*)node)) {
            const size_t attrSize = ((size_t)dtohs(attrExt->attributeSize))
                * dtohs(attrExt->attributeCount);
            if ((dtohs(attrExt->attributeStart)+attrSize) <= (size-headerSize)) {
                return NO_ERROR;
            }
            ALOGW("Bad XML block: node attributes use 0x%x bytes, only have 0x%x bytes\n",
                    (unsigned int)(dtohs(attrExt->attributeStart)+attrSize),
                    (unsigned int)(size-headerSize));
        }
        else {
            ALOGW("Bad XML start block: node header size 0x%x, size 0x%x\n",
                (unsigned int)headerSize, (unsigned int)size);
        }
        return BAD_TYPE;
    }

    return err;

#if 0
    const bool isStart = dtohs(node->header.type) == RES_XML_START_ELEMENT_TYPE;

    const uint16_t headerSize = dtohs(node->header.headerSize);
    const uint32_t size = dtohl(node->header.size);

    if (headerSize >= (isStart ? sizeof(ResXMLTree_attrNode) : sizeof(ResXMLTree_node))) {
        if (size >= headerSize) {
            if (((const uint8_t*)node) <= (mDataEnd-size)) {
                if (!isStart) {
                    return NO_ERROR;
                }
                if ((((size_t)dtohs(node->attributeSize))*dtohs(node->attributeCount))
                        <= (size-headerSize)) {
                    return NO_ERROR;
                }
                ALOGW("Bad XML block: node attributes use 0x%x bytes, only have 0x%x bytes\n",
                        ((int)dtohs(node->attributeSize))*dtohs(node->attributeCount),
                        (int)(size-headerSize));
                return BAD_TYPE;
            }
            ALOGW("Bad XML block: node at 0x%x extends beyond data end 0x%x\n",
                    (int)(((const uint8_t*)node)-((const uint8_t*)mHeader)), (int)mSize);
            return BAD_TYPE;
        }
        ALOGW("Bad XML block: node at 0x%x header size 0x%x smaller than total size 0x%x\n",
                (int)(((const uint8_t*)node)-((const uint8_t*)mHeader)),
                (int)headerSize, (int)size);
        return BAD_TYPE;
    }
    ALOGW("Bad XML block: node at 0x%x header size 0x%x too small\n",
            (int)(((const uint8_t*)node)-((const uint8_t*)mHeader)),
            (int)headerSize);
    return BAD_TYPE;
#endif
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------

void ResTable_config::copyFromDeviceNoSwap(const ResTable_config& o) {
    const size_t size = dtohl(o.size);
    if (size >= sizeof(ResTable_config)) {
        *this = o;
    } else {
        memcpy(this, &o, size);
        memset(((uint8_t*)this)+size, 0, sizeof(ResTable_config)-size);
    }
}

/* static */ size_t unpackLanguageOrRegion(const char in[2], const char base,
        char out[4]) {
  if (in[0] & 0x80) {
      // The high bit is "1", which means this is a packed three letter
      // language code.

      // The smallest 5 bits of the second char are the first alphabet.
      const uint8_t first = in[1] & 0x1f;
      // The last three bits of the second char and the first two bits
      // of the first char are the second alphabet.
      const uint8_t second = ((in[1] & 0xe0) >> 5) + ((in[0] & 0x03) << 3);
      // Bits 3 to 7 (inclusive) of the first char are the third alphabet.
      const uint8_t third = (in[0] & 0x7c) >> 2;

      out[0] = first + base;
      out[1] = second + base;
      out[2] = third + base;
      out[3] = 0;

      return 3;
  }

  if (in[0]) {
      memcpy(out, in, 2);
      memset(out + 2, 0, 2);
      return 2;
  }

  memset(out, 0, 4);
  return 0;
}

/* static */ void packLanguageOrRegion(const char* in, const char base,
        char out[2]) {
  if (in[2] == 0 || in[2] == '-') {
      out[0] = in[0];
      out[1] = in[1];
  } else {
      uint8_t first = (in[0] - base) & 0x007f;
      uint8_t second = (in[1] - base) & 0x007f;
      uint8_t third = (in[2] - base) & 0x007f;

      out[0] = (0x80 | (third << 2) | (second >> 3));
      out[1] = ((second << 5) | first);
  }
}


void ResTable_config::packLanguage(const char* language) {
    packLanguageOrRegion(language, 'a', this->language);
}

void ResTable_config::packRegion(const char* region) {
    packLanguageOrRegion(region, '0', this->country);
}

size_t ResTable_config::unpackLanguage(char language[4]) const {
    return unpackLanguageOrRegion(this->language, 'a', language);
}

size_t ResTable_config::unpackRegion(char region[4]) const {
    return unpackLanguageOrRegion(this->country, '0', region);
}


void ResTable_config::copyFromDtoH(const ResTable_config& o) {
    copyFromDeviceNoSwap(o);
    size = sizeof(ResTable_config);
    mcc = dtohs(mcc);
    mnc = dtohs(mnc);
    density = dtohs(density);
    screenWidth = dtohs(screenWidth);
    screenHeight = dtohs(screenHeight);
    sdkVersion = dtohs(sdkVersion);
    minorVersion = dtohs(minorVersion);
    smallestScreenWidthDp = dtohs(smallestScreenWidthDp);
    screenWidthDp = dtohs(screenWidthDp);
    screenHeightDp = dtohs(screenHeightDp);
}

void ResTable_config::swapHtoD() {
    size = htodl(size);
    mcc = htods(mcc);
    mnc = htods(mnc);
    density = htods(density);
    screenWidth = htods(screenWidth);
    screenHeight = htods(screenHeight);
    sdkVersion = htods(sdkVersion);
    minorVersion = htods(minorVersion);
    smallestScreenWidthDp = htods(smallestScreenWidthDp);
    screenWidthDp = htods(screenWidthDp);
    screenHeightDp = htods(screenHeightDp);
}

/* static */ inline int compareLocales(const ResTable_config &l, const ResTable_config &r) {
    if (l.locale != r.locale) {
        // NOTE: This is the old behaviour with respect to comparison orders.
        // The diff value here doesn't make much sense (given our bit packing scheme)
        // but it's stable, and that's all we need.
        return l.locale - r.locale;
    }

    // The language & region are equal, so compare the scripts and variants.
    int script = memcmp(l.localeScript, r.localeScript, sizeof(l.localeScript));
    if (script) {
        return script;
    }

    // The language, region and script are equal, so compare variants.
    //
    // This should happen very infrequently (if at all.)
    return memcmp(l.localeVariant, r.localeVariant, sizeof(l.localeVariant));
}

int ResTable_config::compare(const ResTable_config& o) const {
    int32_t diff = (int32_t)(imsi - o.imsi);
    if (diff != 0) return diff;
    diff = compareLocales(*this, o);
    if (diff != 0) return diff;
    diff = (int32_t)(screenType - o.screenType);
    if (diff != 0) return diff;
    diff = (int32_t)(input - o.input);
    if (diff != 0) return diff;
    diff = (int32_t)(screenSize - o.screenSize);
    if (diff != 0) return diff;
    diff = (int32_t)(version - o.version);
    if (diff != 0) return diff;
    diff = (int32_t)(screenLayout - o.screenLayout);
    if (diff != 0) return diff;
    diff = (int32_t)(screenLayout2 - o.screenLayout2);
    if (diff != 0) return diff;
    diff = (int32_t)(colorMode - o.colorMode);
    if (diff != 0) return diff;
    diff = (int32_t)(uiMode - o.uiMode);
    if (diff != 0) return diff;
    diff = (int32_t)(smallestScreenWidthDp - o.smallestScreenWidthDp);
    if (diff != 0) return diff;
    diff = (int32_t)(screenSizeDp - o.screenSizeDp);
    return (int)diff;
}

int ResTable_config::compareLogical(const ResTable_config& o) const {
    if (mcc != o.mcc) {
        return mcc < o.mcc ? -1 : 1;
    }
    if (mnc != o.mnc) {
        return mnc < o.mnc ? -1 : 1;
    }

    int diff = compareLocales(*this, o);
    if (diff < 0) {
        return -1;
    }
    if (diff > 0) {
        return 1;
    }

    if ((screenLayout & MASK_LAYOUTDIR) != (o.screenLayout & MASK_LAYOUTDIR)) {
        return (screenLayout & MASK_LAYOUTDIR) < (o.screenLayout & MASK_LAYOUTDIR) ? -1 : 1;
    }
    if (smallestScreenWidthDp != o.smallestScreenWidthDp) {
        return smallestScreenWidthDp < o.smallestScreenWidthDp ? -1 : 1;
    }
    if (screenWidthDp != o.screenWidthDp) {
        return screenWidthDp < o.screenWidthDp ? -1 : 1;
    }
    if (screenHeightDp != o.screenHeightDp) {
        return screenHeightDp < o.screenHeightDp ? -1 : 1;
    }
    if (screenWidth != o.screenWidth) {
        return screenWidth < o.screenWidth ? -1 : 1;
    }
    if (screenHeight != o.screenHeight) {
        return screenHeight < o.screenHeight ? -1 : 1;
    }
    if (density != o.density) {
        return density < o.density ? -1 : 1;
    }
    if (orientation != o.orientation) {
        return orientation < o.orientation ? -1 : 1;
    }
    if (touchscreen != o.touchscreen) {
        return touchscreen < o.touchscreen ? -1 : 1;
    }
    if (input != o.input) {
        return input < o.input ? -1 : 1;
    }
    if (screenLayout != o.screenLayout) {
        return screenLayout < o.screenLayout ? -1 : 1;
    }
    if (uiMode != o.uiMode) {
        return uiMode < o.uiMode ? -1 : 1;
    }
    if (version != o.version) {
        return version < o.version ? -1 : 1;
    }
    return 0;
}

int ResTable_config::diff(const ResTable_config& o) const {
    int diffs = 0;
    if (mcc != o.mcc) diffs |= CONFIG_MCC;
    if (mnc != o.mnc) diffs |= CONFIG_MNC;
    if (orientation != o.orientation) diffs |= CONFIG_ORIENTATION;
    if (density != o.density) diffs |= CONFIG_DENSITY;
    if (touchscreen != o.touchscreen) diffs |= CONFIG_TOUCHSCREEN;
    if (((inputFlags^o.inputFlags)&(MASK_KEYSHIDDEN|MASK_NAVHIDDEN)) != 0)
            diffs |= CONFIG_KEYBOARD_HIDDEN;
    if (keyboard != o.keyboard) diffs |= CONFIG_KEYBOARD;
    if (navigation != o.navigation) diffs |= CONFIG_NAVIGATION;
    if (screenSize != o.screenSize) diffs |= CONFIG_SCREEN_SIZE;
    if (version != o.version) diffs |= CONFIG_VERSION;
    if ((screenLayout & MASK_LAYOUTDIR) != (o.screenLayout & MASK_LAYOUTDIR)) diffs |= CONFIG_LAYOUTDIR;
    if ((screenLayout & ~MASK_LAYOUTDIR) != (o.screenLayout & ~MASK_LAYOUTDIR)) diffs |= CONFIG_SCREEN_LAYOUT;
    if (uiMode != o.uiMode) diffs |= CONFIG_UI_MODE;
    if (smallestScreenWidthDp != o.smallestScreenWidthDp) diffs |= CONFIG_SMALLEST_SCREEN_SIZE;
    if (screenSizeDp != o.screenSizeDp) diffs |= CONFIG_SCREEN_SIZE;

    const int diff = compareLocales(*this, o);
    if (diff) diffs |= CONFIG_LOCALE;

    return diffs;
}

int ResTable_config::isLocaleMoreSpecificThan(const ResTable_config& o) const {
    if (locale || o.locale) {
        if (language[0] != o.language[0]) {
            if (!language[0]) return -1;
            if (!o.language[0]) return 1;
        }

        if (country[0] != o.country[0]) {
            if (!country[0]) return -1;
            if (!o.country[0]) return 1;
        }
    }

    // There isn't a well specified "importance" order between variants and
    // scripts. We can't easily tell whether, say "en-Latn-US" is more or less
    // specific than "en-US-POSIX".
    //
    // We therefore arbitrarily decide to give priority to variants over
    // scripts since it seems more useful to do so. We will consider
    // "en-US-POSIX" to be more specific than "en-Latn-US".

    const int score = ((localeScript[0] != 0) ? 1 : 0) +
        ((localeVariant[0] != 0) ? 2 : 0);

    const int oScore = ((o.localeScript[0] != 0) ? 1 : 0) +
        ((o.localeVariant[0] != 0) ? 2 : 0);

    return score - oScore;

}

bool ResTable_config::isMoreSpecificThan(const ResTable_config& o) const {
    // The order of the following tests defines the importance of one
    // configuration parameter over another.  Those tests first are more
    // important, trumping any values in those following them.
    if (imsi || o.imsi) {
        if (mcc != o.mcc) {
            if (!mcc) return false;
            if (!o.mcc) return true;
        }

        if (mnc != o.mnc) {
            if (!mnc) return false;
            if (!o.mnc) return true;
        }
    }

    if (locale || o.locale) {
        const int diff = isLocaleMoreSpecificThan(o);
        if (diff < 0) {
            return false;
        }

        if (diff > 0) {
            return true;
        }
    }

    if (screenLayout || o.screenLayout) {
        if (((screenLayout^o.screenLayout) & MASK_LAYOUTDIR) != 0) {
            if (!(screenLayout & MASK_LAYOUTDIR)) return false;
            if (!(o.screenLayout & MASK_LAYOUTDIR)) return true;
        }
    }

    if (smallestScreenWidthDp || o.smallestScreenWidthDp) {
        if (smallestScreenWidthDp != o.smallestScreenWidthDp) {
            if (!smallestScreenWidthDp) return false;
            if (!o.smallestScreenWidthDp) return true;
        }
    }

    if (screenSizeDp || o.screenSizeDp) {
        if (screenWidthDp != o.screenWidthDp) {
            if (!screenWidthDp) return false;
            if (!o.screenWidthDp) return true;
        }

        if (screenHeightDp != o.screenHeightDp) {
            if (!screenHeightDp) return false;
            if (!o.screenHeightDp) return true;
        }
    }

    if (screenLayout || o.screenLayout) {
        if (((screenLayout^o.screenLayout) & MASK_SCREENSIZE) != 0) {
            if (!(screenLayout & MASK_SCREENSIZE)) return false;
            if (!(o.screenLayout & MASK_SCREENSIZE)) return true;
        }
        if (((screenLayout^o.screenLayout) & MASK_SCREENLONG) != 0) {
            if (!(screenLayout & MASK_SCREENLONG)) return false;
            if (!(o.screenLayout & MASK_SCREENLONG)) return true;
        }
    }

    if (orientation != o.orientation) {
        if (!orientation) return false;
        if (!o.orientation) return true;
    }

    if (uiMode || o.uiMode) {
        if (((uiMode^o.uiMode) & MASK_UI_MODE_TYPE) != 0) {
            if (!(uiMode & MASK_UI_MODE_TYPE)) return false;
            if (!(o.uiMode & MASK_UI_MODE_TYPE)) return true;
        }
        if (((uiMode^o.uiMode) & MASK_UI_MODE_NIGHT) != 0) {
            if (!(uiMode & MASK_UI_MODE_NIGHT)) return false;
            if (!(o.uiMode & MASK_UI_MODE_NIGHT)) return true;
        }
    }

    // density is never 'more specific'
    // as the default just equals 160

    if (touchscreen != o.touchscreen) {
        if (!touchscreen) return false;
        if (!o.touchscreen) return true;
    }

    if (input || o.input) {
        if (((inputFlags^o.inputFlags) & MASK_KEYSHIDDEN) != 0) {
            if (!(inputFlags & MASK_KEYSHIDDEN)) return false;
            if (!(o.inputFlags & MASK_KEYSHIDDEN)) return true;
        }

        if (((inputFlags^o.inputFlags) & MASK_NAVHIDDEN) != 0) {
            if (!(inputFlags & MASK_NAVHIDDEN)) return false;
            if (!(o.inputFlags & MASK_NAVHIDDEN)) return true;
        }

        if (keyboard != o.keyboard) {
            if (!keyboard) return false;
            if (!o.keyboard) return true;
        }

        if (navigation != o.navigation) {
            if (!navigation) return false;
            if (!o.navigation) return true;
        }
    }

    if (screenSize || o.screenSize) {
        if (screenWidth != o.screenWidth) {
            if (!screenWidth) return false;
            if (!o.screenWidth) return true;
        }

        if (screenHeight != o.screenHeight) {
            if (!screenHeight) return false;
            if (!o.screenHeight) return true;
        }
    }

    if (version || o.version) {
        if (sdkVersion != o.sdkVersion) {
            if (!sdkVersion) return false;
            if (!o.sdkVersion) return true;
        }

        if (minorVersion != o.minorVersion) {
            if (!minorVersion) return false;
            if (!o.minorVersion) return true;
        }
    }
    return false;
}

bool ResTable_config::isBetterThan(const ResTable_config& o,
        const ResTable_config* requested) const {
    if (requested) {
        if (imsi || o.imsi) {
            if ((mcc != o.mcc) && requested->mcc) {
                return (mcc);
            }

            if ((mnc != o.mnc) && requested->mnc) {
                return (mnc);
            }
        }

        if (locale || o.locale) {
            if ((language[0] != o.language[0]) && requested->language[0]) {
                return (language[0]);
            }

            if ((country[0] != o.country[0]) && requested->country[0]) {
                return (country[0]);
            }
        }

        if (localeScript[0] || o.localeScript[0]) {
            if (localeScript[0] != o.localeScript[0] && requested->localeScript[0]) {
                return localeScript[0];
            }
        }

        if (localeVariant[0] || o.localeVariant[0]) {
            if (localeVariant[0] != o.localeVariant[0] && requested->localeVariant[0]) {
                return localeVariant[0];
            }
        }

        if (screenLayout || o.screenLayout) {
            if (((screenLayout^o.screenLayout) & MASK_LAYOUTDIR) != 0
                    && (requested->screenLayout & MASK_LAYOUTDIR)) {
                int myLayoutDir = screenLayout & MASK_LAYOUTDIR;
                int oLayoutDir = o.screenLayout & MASK_LAYOUTDIR;
                return (myLayoutDir > oLayoutDir);
            }
        }

        if (smallestScreenWidthDp || o.smallestScreenWidthDp) {
            // The configuration closest to the actual size is best.
            // We assume that larger configs have already been filtered
            // out at this point.  That means we just want the largest one.
            if (smallestScreenWidthDp != o.smallestScreenWidthDp) {
                return smallestScreenWidthDp > o.smallestScreenWidthDp;
            }
        }

        if (screenSizeDp || o.screenSizeDp) {
            // "Better" is based on the sum of the difference between both
            // width and height from the requested dimensions.  We are
            // assuming the invalid configs (with smaller dimens) have
            // already been filtered.  Note that if a particular dimension
            // is unspecified, we will end up with a large value (the
            // difference between 0 and the requested dimension), which is
            // good since we will prefer a config that has specified a
            // dimension value.
            int myDelta = 0, otherDelta = 0;
            if (requested->screenWidthDp) {
                myDelta += requested->screenWidthDp - screenWidthDp;
                otherDelta += requested->screenWidthDp - o.screenWidthDp;
            }
            if (requested->screenHeightDp) {
                myDelta += requested->screenHeightDp - screenHeightDp;
                otherDelta += requested->screenHeightDp - o.screenHeightDp;
            }
            if (kDebugTableSuperNoisy) {
                ALOGI("Comparing this %dx%d to other %dx%d in %dx%d: myDelta=%d otherDelta=%d",
                        screenWidthDp, screenHeightDp, o.screenWidthDp, o.screenHeightDp,
                        requested->screenWidthDp, requested->screenHeightDp, myDelta, otherDelta);
            }
            if (myDelta != otherDelta) {
                return myDelta < otherDelta;
            }
        }

        if (screenLayout || o.screenLayout) {
            if (((screenLayout^o.screenLayout) & MASK_SCREENSIZE) != 0
                    && (requested->screenLayout & MASK_SCREENSIZE)) {
                // A little backwards compatibility here: undefined is
                // considered equivalent to normal.  But only if the
                // requested size is at least normal; otherwise, small
                // is better than the default.
                int mySL = (screenLayout & MASK_SCREENSIZE);
                int oSL = (o.screenLayout & MASK_SCREENSIZE);
                int fixedMySL = mySL;
                int fixedOSL = oSL;
                if ((requested->screenLayout & MASK_SCREENSIZE) >= SCREENSIZE_NORMAL) {
                    if (fixedMySL == 0) fixedMySL = SCREENSIZE_NORMAL;
                    if (fixedOSL == 0) fixedOSL = SCREENSIZE_NORMAL;
                }
                // For screen size, the best match is the one that is
                // closest to the requested screen size, but not over
                // (the not over part is dealt with in match() below).
                if (fixedMySL == fixedOSL) {
                    // If the two are the same, but 'this' is actually
                    // undefined, then the other is really a better match.
                    if (mySL == 0) return false;
                    return true;
                }
                if (fixedMySL != fixedOSL) {
                    return fixedMySL > fixedOSL;
                }
            }
            if (((screenLayout^o.screenLayout) & MASK_SCREENLONG) != 0
                    && (requested->screenLayout & MASK_SCREENLONG)) {
                return (screenLayout & MASK_SCREENLONG);
            }
        }

        if ((orientation != o.orientation) && requested->orientation) {
            return (orientation);
        }

        if (uiMode || o.uiMode) {
            if (((uiMode^o.uiMode) & MASK_UI_MODE_TYPE) != 0
                    && (requested->uiMode & MASK_UI_MODE_TYPE)) {
                return (uiMode & MASK_UI_MODE_TYPE);
            }
            if (((uiMode^o.uiMode) & MASK_UI_MODE_NIGHT) != 0
                    && (requested->uiMode & MASK_UI_MODE_NIGHT)) {
                return (uiMode & MASK_UI_MODE_NIGHT);
            }
        }

        if (screenType || o.screenType) {
            if (density != o.density) {
                // Use the system default density (DENSITY_MEDIUM, 160dpi) if none specified.
                const int thisDensity = density ? density : int(ResTable_config::DENSITY_MEDIUM);
                const int otherDensity = o.density ? o.density : int(ResTable_config::DENSITY_MEDIUM);

                // We always prefer DENSITY_ANY over scaling a density bucket.
                if (thisDensity == ResTable_config::DENSITY_ANY) {
                    return true;
                } else if (otherDensity == ResTable_config::DENSITY_ANY) {
                    return false;
                }

                int requestedDensity = requested->density;
                if (requested->density == 0 ||
                        requested->density == ResTable_config::DENSITY_ANY) {
                    requestedDensity = ResTable_config::DENSITY_MEDIUM;
                }

                // DENSITY_ANY is now dealt with. We should look to
                // pick a density bucket and potentially scale it.
                // Any density is potentially useful
                // because the system will scale it.  Scaling down
                // is generally better than scaling up.
                int h = thisDensity;
                int l = otherDensity;
                bool bImBigger = true;
                if (l > h) {
                    int t = h;
                    h = l;
                    l = t;
                    bImBigger = false;
                }

                if (requestedDensity >= h) {
                    // requested value higher than both l and h, give h
                    return bImBigger;
                }
                if (l >= requestedDensity) {
                    // requested value lower than both l and h, give l
                    return !bImBigger;
                }
                // saying that scaling down is 2x better than up
                if (((2 * l) - requestedDensity) * h > requestedDensity * requestedDensity) {
                    return !bImBigger;
                } else {
                    return bImBigger;
                }
            }

            if ((touchscreen != o.touchscreen) && requested->touchscreen) {
                return (touchscreen);
            }
        }

        if (input || o.input) {
            const int keysHidden = inputFlags & MASK_KEYSHIDDEN;
            const int oKeysHidden = o.inputFlags & MASK_KEYSHIDDEN;
            if (keysHidden != oKeysHidden) {
                const int reqKeysHidden =
                        requested->inputFlags & MASK_KEYSHIDDEN;
                if (reqKeysHidden) {

                    if (!keysHidden) return false;
                    if (!oKeysHidden) return true;
                    // For compatibility, we count KEYSHIDDEN_NO as being
                    // the same as KEYSHIDDEN_SOFT.  Here we disambiguate
                    // these by making an exact match more specific.
                    if (reqKeysHidden == keysHidden) return true;
                    if (reqKeysHidden == oKeysHidden) return false;
                }
            }

            const int navHidden = inputFlags & MASK_NAVHIDDEN;
            const int oNavHidden = o.inputFlags & MASK_NAVHIDDEN;
            if (navHidden != oNavHidden) {
                const int reqNavHidden =
                        requested->inputFlags & MASK_NAVHIDDEN;
                if (reqNavHidden) {

                    if (!navHidden) return false;
                    if (!oNavHidden) return true;
                }
            }

            if ((keyboard != o.keyboard) && requested->keyboard) {
                return (keyboard);
            }

            if ((navigation != o.navigation) && requested->navigation) {
                return (navigation);
            }
        }

        if (screenSize || o.screenSize) {
            // "Better" is based on the sum of the difference between both
            // width and height from the requested dimensions.  We are
            // assuming the invalid configs (with smaller sizes) have
            // already been filtered.  Note that if a particular dimension
            // is unspecified, we will end up with a large value (the
            // difference between 0 and the requested dimension), which is
            // good since we will prefer a config that has specified a
            // size value.
            int myDelta = 0, otherDelta = 0;
            if (requested->screenWidth) {
                myDelta += requested->screenWidth - screenWidth;
                otherDelta += requested->screenWidth - o.screenWidth;
            }
            if (requested->screenHeight) {
                myDelta += requested->screenHeight - screenHeight;
                otherDelta += requested->screenHeight - o.screenHeight;
            }
            if (myDelta != otherDelta) {
                return myDelta < otherDelta;
            }
        }

        if (version || o.version) {
            if ((sdkVersion != o.sdkVersion) && requested->sdkVersion) {
                return (sdkVersion > o.sdkVersion);
            }

            if ((minorVersion != o.minorVersion) &&
                    requested->minorVersion) {
                return (minorVersion);
            }
        }

        return false;
    }
    return isMoreSpecificThan(o);
}

bool ResTable_config::match(const ResTable_config& settings) const {
    if (imsi != 0) {
        if (mcc != 0 && mcc != settings.mcc) {
            return false;
        }
        if (mnc != 0 && mnc != settings.mnc) {
            return false;
        }
    }
    if (locale != 0) {
        // Don't consider the script & variants when deciding matches.
        //
        // If we two configs differ only in their script or language, they
        // can be weeded out in the isMoreSpecificThan test.
        if (language[0] != 0
            && (language[0] != settings.language[0]
                || language[1] != settings.language[1])) {
            return false;
        }

        if (country[0] != 0
            && (country[0] != settings.country[0]
                || country[1] != settings.country[1])) {
            return false;
        }
    }

    if (screenConfig != 0) {
        const int layoutDir = screenLayout&MASK_LAYOUTDIR;
        const int setLayoutDir = settings.screenLayout&MASK_LAYOUTDIR;
        if (layoutDir != 0 && layoutDir != setLayoutDir) {
            return false;
        }

        const int screenSize = screenLayout&MASK_SCREENSIZE;
        const int setScreenSize = settings.screenLayout&MASK_SCREENSIZE;
        // Any screen sizes for larger screens than the setting do not
        // match.
        if (screenSize != 0 && screenSize > setScreenSize) {
            return false;
        }

        const int screenLong = screenLayout&MASK_SCREENLONG;
        const int setScreenLong = settings.screenLayout&MASK_SCREENLONG;
        if (screenLong != 0 && screenLong != setScreenLong) {
            return false;
        }

        const int uiModeType = uiMode&MASK_UI_MODE_TYPE;
        const int setUiModeType = settings.uiMode&MASK_UI_MODE_TYPE;
        if (uiModeType != 0 && uiModeType != setUiModeType) {
            return false;
        }

        const int uiModeNight = uiMode&MASK_UI_MODE_NIGHT;
        const int setUiModeNight = settings.uiMode&MASK_UI_MODE_NIGHT;
        if (uiModeNight != 0 && uiModeNight != setUiModeNight) {
            return false;
        }

        if (smallestScreenWidthDp != 0
                && smallestScreenWidthDp > settings.smallestScreenWidthDp) {
            return false;
        }
    }
    if (screenSizeDp != 0) {
        if (screenWidthDp != 0 && screenWidthDp > settings.screenWidthDp) {
            if (kDebugTableSuperNoisy) {
                ALOGI("Filtering out width %d in requested %d", screenWidthDp,
                        settings.screenWidthDp);
            }
            return false;
        }
        if (screenHeightDp != 0 && screenHeightDp > settings.screenHeightDp) {
            if (kDebugTableSuperNoisy) {
                ALOGI("Filtering out height %d in requested %d", screenHeightDp,
                        settings.screenHeightDp);
            }
            return false;
        }
    }
    if (screenType != 0) {
        if (orientation != 0 && orientation != settings.orientation) {
            return false;
        }
        // density always matches - we can scale it.  See isBetterThan
        if (touchscreen != 0 && touchscreen != settings.touchscreen) {
            return false;
        }
    }
    if (input != 0) {
        const int keysHidden = inputFlags&MASK_KEYSHIDDEN;
        const int setKeysHidden = settings.inputFlags&MASK_KEYSHIDDEN;
        if (keysHidden != 0 && keysHidden != setKeysHidden) {
            // For compatibility, we count a request for KEYSHIDDEN_NO as also
            // matching the more recent KEYSHIDDEN_SOFT.  Basically
            // KEYSHIDDEN_NO means there is some kind of keyboard available.
            if (kDebugTableSuperNoisy) {
                ALOGI("Matching keysHidden: have=%d, config=%d\n", keysHidden, setKeysHidden);
            }
            if (keysHidden != KEYSHIDDEN_NO || setKeysHidden != KEYSHIDDEN_SOFT) {
                if (kDebugTableSuperNoisy) {
                    ALOGI("No match!");
                }
                return false;
            }
        }
        const int navHidden = inputFlags&MASK_NAVHIDDEN;
        const int setNavHidden = settings.inputFlags&MASK_NAVHIDDEN;
        if (navHidden != 0 && navHidden != setNavHidden) {
            return false;
        }
        if (keyboard != 0 && keyboard != settings.keyboard) {
            return false;
        }
        if (navigation != 0 && navigation != settings.navigation) {
            return false;
        }
    }
    if (screenSize != 0) {
        if (screenWidth != 0 && screenWidth > settings.screenWidth) {
            return false;
        }
        if (screenHeight != 0 && screenHeight > settings.screenHeight) {
            return false;
        }
    }
    if (version != 0) {
        if (sdkVersion != 0 && sdkVersion > settings.sdkVersion) {
            return false;
        }
        if (minorVersion != 0 && minorVersion != settings.minorVersion) {
            return false;
        }
    }
    return true;
}

void ResTable_config::getBcp47Locale(char str[RESTABLE_MAX_LOCALE_LEN]) const {
    memset(str, 0, RESTABLE_MAX_LOCALE_LEN);

    // This represents the "any" locale value, which has traditionally been
    // represented by the empty string.
    if (!language[0] && !country[0]) {
        return;
    }

    size_t charsWritten = 0;
    if (language[0]) {
        charsWritten += unpackLanguage(str);
    }

    if (localeScript[0]) {
        if (charsWritten) {
            str[charsWritten++] = '-';
        }
        memcpy(str + charsWritten, localeScript, sizeof(localeScript));
        charsWritten += sizeof(localeScript);
    }

    if (country[0]) {
        if (charsWritten) {
            str[charsWritten++] = '-';
        }
        charsWritten += unpackRegion(str + charsWritten);
    }

    if (localeVariant[0]) {
        if (charsWritten) {
            str[charsWritten++] = '-';
        }
        memcpy(str + charsWritten, localeVariant, sizeof(localeVariant));
    }
}

/* static */ inline bool assignLocaleComponent(ResTable_config* config,
        const char* start, size_t size) {

  switch (size) {
       case 0:
           return false;
       case 2:
       case 3:
           config->language[0] ? config->packRegion(start) : config->packLanguage(start);
           break;
       case 4:
           config->localeScript[0] = toupper(start[0]);
           for (size_t i = 1; i < 4; ++i) {
               config->localeScript[i] = tolower(start[i]);
           }
           break;
       case 5:
       case 6:
       case 7:
       case 8:
           for (size_t i = 0; i < size; ++i) {
               config->localeVariant[i] = tolower(start[i]);
           }
           break;
       default:
           return false;
  }

  return true;
}

void ResTable_config::setBcp47Locale(const char* in) {
    locale = 0;
    memset(localeScript, 0, sizeof(localeScript));
    memset(localeVariant, 0, sizeof(localeVariant));

    const char* separator = in;
    const char* start = in;
    while ((separator = strchr(start, '-')) != NULL) {
        const size_t size = separator - start;
        if (!assignLocaleComponent(this, start, size)) {
            fprintf(stderr, "Invalid BCP-47 locale string: %s", in);
        }

        start = (separator + 1);
    }

    const size_t size = in + strlen(in) - start;
    assignLocaleComponent(this, start, size);
}

String8 ResTable_config::toString() const {
    String8 res;

    if (mcc != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("mcc%d", dtohs(mcc));
    }
    if (mnc != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("mnc%d", dtohs(mnc));
    }

    char localeStr[RESTABLE_MAX_LOCALE_LEN];
    getBcp47Locale(localeStr);
    if (strlen(localeStr) > 0) {
        if (res.size() > 0) res.append("-");
        res.append(localeStr);
    }

    if ((screenLayout&MASK_LAYOUTDIR) != 0) {
        if (res.size() > 0) res.append("-");
        switch (screenLayout&ResTable_config::MASK_LAYOUTDIR) {
            case ResTable_config::LAYOUTDIR_LTR:
                res.append("ldltr");
                break;
            case ResTable_config::LAYOUTDIR_RTL:
                res.append("ldrtl");
                break;
            default:
                res.appendFormat("layoutDir=%d",
                        dtohs(screenLayout&ResTable_config::MASK_LAYOUTDIR));
                break;
        }
    }
    if (smallestScreenWidthDp != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("sw%ddp", dtohs(smallestScreenWidthDp));
    }
    if (screenWidthDp != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("w%ddp", dtohs(screenWidthDp));
    }
    if (screenHeightDp != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("h%ddp", dtohs(screenHeightDp));
    }
    if ((screenLayout&MASK_SCREENSIZE) != SCREENSIZE_ANY) {
        if (res.size() > 0) res.append("-");
        switch (screenLayout&ResTable_config::MASK_SCREENSIZE) {
            case ResTable_config::SCREENSIZE_SMALL:
                res.append("small");
                break;
            case ResTable_config::SCREENSIZE_NORMAL:
                res.append("normal");
                break;
            case ResTable_config::SCREENSIZE_LARGE:
                res.append("large");
                break;
            case ResTable_config::SCREENSIZE_XLARGE:
                res.append("xlarge");
                break;
            default:
                res.appendFormat("screenLayoutSize=%d",
                        dtohs(screenLayout&ResTable_config::MASK_SCREENSIZE));
                break;
        }
    }
    if ((screenLayout&MASK_SCREENLONG) != 0) {
        if (res.size() > 0) res.append("-");
        switch (screenLayout&ResTable_config::MASK_SCREENLONG) {
            case ResTable_config::SCREENLONG_NO:
                res.append("notlong");
                break;
            case ResTable_config::SCREENLONG_YES:
                res.append("long");
                break;
            default:
                res.appendFormat("screenLayoutLong=%d",
                        dtohs(screenLayout&ResTable_config::MASK_SCREENLONG));
                break;
        }
    }
    if (orientation != ORIENTATION_ANY) {
        if (res.size() > 0) res.append("-");
        switch (orientation) {
            case ResTable_config::ORIENTATION_PORT:
                res.append("port");
                break;
            case ResTable_config::ORIENTATION_LAND:
                res.append("land");
                break;
            case ResTable_config::ORIENTATION_SQUARE:
                res.append("square");
                break;
            default:
                res.appendFormat("orientation=%d", dtohs(orientation));
                break;
        }
    }
    if ((uiMode&MASK_UI_MODE_TYPE) != UI_MODE_TYPE_ANY) {
        if (res.size() > 0) res.append("-");
        switch (uiMode&ResTable_config::MASK_UI_MODE_TYPE) {
            case ResTable_config::UI_MODE_TYPE_DESK:
                res.append("desk");
                break;
            case ResTable_config::UI_MODE_TYPE_CAR:
                res.append("car");
                break;
            case ResTable_config::UI_MODE_TYPE_TELEVISION:
                res.append("television");
                break;
            case ResTable_config::UI_MODE_TYPE_APPLIANCE:
                res.append("appliance");
                break;
            case ResTable_config::UI_MODE_TYPE_WATCH:
                res.append("watch");
                break;
            default:
                res.appendFormat("uiModeType=%d",
                        dtohs(screenLayout&ResTable_config::MASK_UI_MODE_TYPE));
                break;
        }
    }
    if ((uiMode&MASK_UI_MODE_NIGHT) != 0) {
        if (res.size() > 0) res.append("-");
        switch (uiMode&ResTable_config::MASK_UI_MODE_NIGHT) {
            case ResTable_config::UI_MODE_NIGHT_NO:
                res.append("notnight");
                break;
            case ResTable_config::UI_MODE_NIGHT_YES:
                res.append("night");
                break;
            default:
                res.appendFormat("uiModeNight=%d",
                        dtohs(uiMode&MASK_UI_MODE_NIGHT));
                break;
        }
    }
    if (density != DENSITY_DEFAULT) {
        if (res.size() > 0) res.append("-");
        switch (density) {
            case ResTable_config::DENSITY_LOW:
                res.append("ldpi");
                break;
            case ResTable_config::DENSITY_MEDIUM:
                res.append("mdpi");
                break;
            case ResTable_config::DENSITY_TV:
                res.append("tvdpi");
                break;
            case ResTable_config::DENSITY_HIGH:
                res.append("hdpi");
                break;
            case ResTable_config::DENSITY_XHIGH:
                res.append("xhdpi");
                break;
            case ResTable_config::DENSITY_XXHIGH:
                res.append("xxhdpi");
                break;
            case ResTable_config::DENSITY_XXXHIGH:
                res.append("xxxhdpi");
                break;
            case ResTable_config::DENSITY_NONE:
                res.append("nodpi");
                break;
            case ResTable_config::DENSITY_ANY:
                res.append("anydpi");
                break;
            default:
                res.appendFormat("%ddpi", dtohs(density));
                break;
        }
    }
    if (touchscreen != TOUCHSCREEN_ANY) {
        if (res.size() > 0) res.append("-");
        switch (touchscreen) {
            case ResTable_config::TOUCHSCREEN_NOTOUCH:
                res.append("notouch");
                break;
            case ResTable_config::TOUCHSCREEN_FINGER:
                res.append("finger");
                break;
            case ResTable_config::TOUCHSCREEN_STYLUS:
                res.append("stylus");
                break;
            default:
                res.appendFormat("touchscreen=%d", dtohs(touchscreen));
                break;
        }
    }
    if ((inputFlags&MASK_KEYSHIDDEN) != 0) {
        if (res.size() > 0) res.append("-");
        switch (inputFlags&MASK_KEYSHIDDEN) {
            case ResTable_config::KEYSHIDDEN_NO:
                res.append("keysexposed");
                break;
            case ResTable_config::KEYSHIDDEN_YES:
                res.append("keyshidden");
                break;
            case ResTable_config::KEYSHIDDEN_SOFT:
                res.append("keyssoft");
                break;
        }
    }
    if (keyboard != KEYBOARD_ANY) {
        if (res.size() > 0) res.append("-");
        switch (keyboard) {
            case ResTable_config::KEYBOARD_NOKEYS:
                res.append("nokeys");
                break;
            case ResTable_config::KEYBOARD_QWERTY:
                res.append("qwerty");
                break;
            case ResTable_config::KEYBOARD_12KEY:
                res.append("12key");
                break;
            default:
                res.appendFormat("keyboard=%d", dtohs(keyboard));
                break;
        }
    }
    if ((inputFlags&MASK_NAVHIDDEN) != 0) {
        if (res.size() > 0) res.append("-");
        switch (inputFlags&MASK_NAVHIDDEN) {
            case ResTable_config::NAVHIDDEN_NO:
                res.append("navexposed");
                break;
            case ResTable_config::NAVHIDDEN_YES:
                res.append("navhidden");
                break;
            default:
                res.appendFormat("inputFlagsNavHidden=%d",
                        dtohs(inputFlags&MASK_NAVHIDDEN));
                break;
        }
    }
    if (navigation != NAVIGATION_ANY) {
        if (res.size() > 0) res.append("-");
        switch (navigation) {
            case ResTable_config::NAVIGATION_NONAV:
                res.append("nonav");
                break;
            case ResTable_config::NAVIGATION_DPAD:
                res.append("dpad");
                break;
            case ResTable_config::NAVIGATION_TRACKBALL:
                res.append("trackball");
                break;
            case ResTable_config::NAVIGATION_WHEEL:
                res.append("wheel");
                break;
            default:
                res.appendFormat("navigation=%d", dtohs(navigation));
                break;
        }
    }
    if (screenSize != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("%dx%d", dtohs(screenWidth), dtohs(screenHeight));
    }
    if (version != 0) {
        if (res.size() > 0) res.append("-");
        res.appendFormat("v%d", dtohs(sdkVersion));
        if (minorVersion != 0) {
            res.appendFormat(".%d", dtohs(minorVersion));
        }
    }

    return res;
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------


DynamicRefTable::DynamicRefTable(uint8_t packageId)
    : mAssignedPackageId(packageId)
{
    memset(mLookupTable, 0, sizeof(mLookupTable));

    // Reserved package ids
    mLookupTable[APP_PACKAGE_ID] = APP_PACKAGE_ID;
    mLookupTable[SYS_PACKAGE_ID] = SYS_PACKAGE_ID;
}

status_t DynamicRefTable::load(const ResTable_lib_header* const header)
{
    const uint32_t entryCount = dtohl(header->count);
    const uint32_t sizeOfEntries = sizeof(ResTable_lib_entry) * entryCount;
    const uint32_t expectedSize = dtohl(header->header.size) - dtohl(header->header.headerSize);
    if (sizeOfEntries > expectedSize) {
        ALOGE("ResTable_lib_header size %u is too small to fit %u entries (x %u).",
                expectedSize, entryCount, (uint32_t)sizeof(ResTable_lib_entry));
        return UNKNOWN_ERROR;
    }

    const ResTable_lib_entry* entry = (const ResTable_lib_entry*)(((uint8_t*) header) +
            dtohl(header->header.headerSize));
    for (uint32_t entryIndex = 0; entryIndex < entryCount; entryIndex++) {
        uint32_t packageId = dtohl(entry->packageId);
        char16_t tmpName[sizeof(entry->packageName) / sizeof(char16_t)];
        strcpy16_dtoh(tmpName, entry->packageName, sizeof(entry->packageName) / sizeof(char16_t));
        if (kDebugLibNoisy) {
            ALOGV("Found lib entry %s with id %d\n", String8(tmpName).string(),
                    dtohl(entry->packageId));
        }
        if (packageId >= 256) {
            ALOGE("Bad package id 0x%08x", packageId);
            return UNKNOWN_ERROR;
        }
        mEntries.replaceValueFor(String16(tmpName), (uint8_t) packageId);
        entry = entry + 1;
    }
    return NO_ERROR;
}

status_t DynamicRefTable::addMappings(const DynamicRefTable& other) {
    if (mAssignedPackageId != other.mAssignedPackageId) {
        return UNKNOWN_ERROR;
    }

    const size_t entryCount = other.mEntries.size();
    for (size_t i = 0; i < entryCount; i++) {
        ssize_t index = mEntries.indexOfKey(other.mEntries.keyAt(i));
        if (index < 0) {
            mEntries.add(other.mEntries.keyAt(i), other.mEntries[i]);
        } else {
            if (other.mEntries[i] != mEntries[index]) {
                return UNKNOWN_ERROR;
            }
        }
    }

    // Merge the lookup table. No entry can conflict
    // (value of 0 means not set).
    for (size_t i = 0; i < 256; i++) {
        if (mLookupTable[i] != other.mLookupTable[i]) {
            if (mLookupTable[i] == 0) {
                mLookupTable[i] = other.mLookupTable[i];
            } else if (other.mLookupTable[i] != 0) {
                return UNKNOWN_ERROR;
            }
        }
    }
    return NO_ERROR;
}

status_t DynamicRefTable::addMapping(const String16& packageName, uint8_t packageId)
{
    ssize_t index = mEntries.indexOfKey(packageName);
    if (index < 0) {
        return UNKNOWN_ERROR;
    }
    mLookupTable[mEntries.valueAt(index)] = packageId;
    return NO_ERROR;
}

status_t DynamicRefTable::lookupResourceId(uint32_t* resId) const {
    uint32_t res = *resId;
    size_t packageId = Res_GETPACKAGE(res) + 1;

    if (packageId == APP_PACKAGE_ID) {
        // No lookup needs to be done, app package IDs are absolute.
        return NO_ERROR;
    }

    if (packageId == 0) {
        // The package ID is 0x00. That means that a shared library is accessing
        // its own local resource, so we fix up the resource with the calling
        // package ID.
        *resId |= ((uint32_t) mAssignedPackageId) << 24;
        return NO_ERROR;
    }

    // Do a proper lookup.
    uint8_t translatedId = mLookupTable[packageId];
    if (translatedId == 0) {
        ALOGV("DynamicRefTable(0x%02x): No mapping for build-time package ID 0x%02x.",
                (uint8_t)mAssignedPackageId, (uint8_t)packageId);
        for (size_t i = 0; i < 256; i++) {
            if (mLookupTable[i] != 0) {
                ALOGV("e[0x%02x] -> 0x%02x", (uint8_t)i, mLookupTable[i]);
            }
        }
        return UNKNOWN_ERROR;
    }

    *resId = (res & 0x00ffffff) | (((uint32_t) translatedId) << 24);
    return NO_ERROR;
}

status_t DynamicRefTable::lookupResourceValue(Res_value* value) const {
    if (value->dataType != Res_value::TYPE_DYNAMIC_REFERENCE) {
        return NO_ERROR;
    }

    status_t err = lookupResourceId(&value->data);
    if (err != NO_ERROR) {
        return err;
    }

    value->dataType = Res_value::TYPE_REFERENCE;
    return NO_ERROR;
}

}   // namespace android
