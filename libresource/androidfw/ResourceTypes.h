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

//
// Definitions of resource data structures.
//
#ifndef _LIBS_UTILS_RESOURCE_TYPES_H
#define _LIBS_UTILS_RESOURCE_TYPES_H

#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/String16.h"
#include "utils/Vector.h"
#include "utils/KeyedVector.h"

#include <utils/threads.h>

#include <stdint.h>
#include <sys/types.h>

#if defined(_MSC_VER) || defined(__MINGW64__) || defined(__MINGW32__)
#include <mutex>
#endif

#include "android/configuration.h"

namespace android {

/**
 * In C++11, char16_t is defined as *at least* 16 bits. We do a lot of
 * casting on raw data and expect char16_t to be exactly 16 bits.
 */
#if __cplusplus >= 201103L
struct __assertChar16Size {
    static_assert(sizeof(char16_t) == sizeof(uint16_t), "char16_t is not 16 bits");
    static_assert(alignof(char16_t) == alignof(uint16_t), "char16_t is not 16-bit aligned");
};
#endif

/** ********************************************************************
 *  Base Types
 *
 *  These are standard types that are shared between multiple specific
 *  resource types.
 *
 *********************************************************************** */

/**
 * Header that appears at the front of every data chunk in a resource.
 */
struct ResChunk_header
{
    // Type identifier for this chunk.  The meaning of this value depends
    // on the containing chunk.
    uint16_t type;

    // Size of the chunk header (in bytes).  Adding this value to
    // the address of the chunk allows you to find its associated data
    // (if any).
    uint16_t headerSize;

    // Total size of this chunk (in bytes).  This is the chunkSize plus
    // the size of any data associated with the chunk.  Adding this value
    // to the chunk allows you to completely skip its contents (including
    // any child chunks).  If this value is the same as chunkSize, there is
    // no data associated with the chunk.
    uint32_t size;
};

enum {
    RES_NULL_TYPE               = 0x0000,
    RES_STRING_POOL_TYPE        = 0x0001,
    RES_TABLE_TYPE              = 0x0002,
    RES_XML_TYPE                = 0x0003,

    // Chunk types in RES_XML_TYPE
    RES_XML_FIRST_CHUNK_TYPE    = 0x0100,
    RES_XML_START_NAMESPACE_TYPE= 0x0100,
    RES_XML_END_NAMESPACE_TYPE  = 0x0101,
    RES_XML_START_ELEMENT_TYPE  = 0x0102,
    RES_XML_END_ELEMENT_TYPE    = 0x0103,
    RES_XML_CDATA_TYPE          = 0x0104,
    RES_XML_LAST_CHUNK_TYPE     = 0x017f,
    // This contains a uint32_t array mapping strings in the string
    // pool back to resource identifiers.  It is optional.
    RES_XML_RESOURCE_MAP_TYPE   = 0x0180,

    // Chunk types in RES_TABLE_TYPE
    RES_TABLE_PACKAGE_TYPE      = 0x0200,
    RES_TABLE_TYPE_TYPE         = 0x0201,
    RES_TABLE_TYPE_SPEC_TYPE    = 0x0202,
    RES_TABLE_LIBRARY_TYPE      = 0x0203
};

/**
 * Macros for building/splitting resource identifiers.
 */
#define Res_VALIDID(resid) (resid != 0)
#define Res_CHECKID(resid) ((resid&0xFFFF0000) != 0)
#define Res_MAKEID(package, type, entry) \
    (((package+1)<<24) | (((type+1)&0xFF)<<16) | (entry&0xFFFF))
#define Res_GETPACKAGE(id) ((id>>24)-1)
#define Res_GETTYPE(id) (((id>>16)&0xFF)-1)
#define Res_GETENTRY(id) (id&0xFFFF)

#define Res_INTERNALID(resid) ((resid&0xFFFF0000) != 0 && (resid&0xFF0000) == 0)
#define Res_MAKEINTERNAL(entry) (0x01000000 | (entry&0xFFFF))
#define Res_MAKEARRAY(entry) (0x02000000 | (entry&0xFFFF))

static const size_t Res_MAXPACKAGE = 255;
static const size_t Res_MAXTYPE = 255;

/**
 * Representation of a value in a resource, supplying type
 * information.
 */
struct Res_value
{
    // Number of bytes in this structure.
    uint16_t size;

    // Always set to 0.
    uint8_t res0;

    // Type of the data value.
    enum {
        // The 'data' is either 0 or 1, specifying this resource is either
        // undefined or empty, respectively.
        TYPE_NULL = 0x00,
        // The 'data' holds a ResTable_ref, a reference to another resource
        // table entry.
        TYPE_REFERENCE = 0x01,
        // The 'data' holds an attribute resource identifier.
        TYPE_ATTRIBUTE = 0x02,
        // The 'data' holds an index into the containing resource table's
        // global value string pool.
        TYPE_STRING = 0x03,
        // The 'data' holds a single-precision floating point number.
        TYPE_FLOAT = 0x04,
        // The 'data' holds a complex number encoding a dimension value,
        // such as "100in".
        TYPE_DIMENSION = 0x05,
        // The 'data' holds a complex number encoding a fraction of a
        // container.
        TYPE_FRACTION = 0x06,
        // The 'data' holds a dynamic ResTable_ref, which needs to be
        // resolved before it can be used like a TYPE_REFERENCE.
        TYPE_DYNAMIC_REFERENCE = 0x07,

        // Beginning of integer flavors...
        TYPE_FIRST_INT = 0x10,

        // The 'data' is a raw integer value of the form n..n.
        TYPE_INT_DEC = 0x10,
        // The 'data' is a raw integer value of the form 0xn..n.
        TYPE_INT_HEX = 0x11,
        // The 'data' is either 0 or 1, for input "false" or "true" respectively.
        TYPE_INT_BOOLEAN = 0x12,

        // Beginning of color integer flavors...
        TYPE_FIRST_COLOR_INT = 0x1c,

        // The 'data' is a raw integer value of the form #aarrggbb.
        TYPE_INT_COLOR_ARGB8 = 0x1c,
        // The 'data' is a raw integer value of the form #rrggbb.
        TYPE_INT_COLOR_RGB8 = 0x1d,
        // The 'data' is a raw integer value of the form #argb.
        TYPE_INT_COLOR_ARGB4 = 0x1e,
        // The 'data' is a raw integer value of the form #rgb.
        TYPE_INT_COLOR_RGB4 = 0x1f,

        // ...end of integer flavors.
        TYPE_LAST_COLOR_INT = 0x1f,

        // ...end of integer flavors.
        TYPE_LAST_INT = 0x1f
    };
    uint8_t dataType;

    // Structure of complex data values (TYPE_UNIT and TYPE_FRACTION)
    enum {
        // Where the unit type information is.  This gives us 16 possible
        // types, as defined below.
        COMPLEX_UNIT_SHIFT = 0,
        COMPLEX_UNIT_MASK = 0xf,

        // TYPE_DIMENSION: Value is raw pixels.
        COMPLEX_UNIT_PX = 0,
        // TYPE_DIMENSION: Value is Device Independent Pixels.
        COMPLEX_UNIT_DIP = 1,
        // TYPE_DIMENSION: Value is a Scaled device independent Pixels.
        COMPLEX_UNIT_SP = 2,
        // TYPE_DIMENSION: Value is in points.
        COMPLEX_UNIT_PT = 3,
        // TYPE_DIMENSION: Value is in inches.
        COMPLEX_UNIT_IN = 4,
        // TYPE_DIMENSION: Value is in millimeters.
        COMPLEX_UNIT_MM = 5,

        // TYPE_FRACTION: A basic fraction of the overall size.
        COMPLEX_UNIT_FRACTION = 0,
        // TYPE_FRACTION: A fraction of the parent size.
        COMPLEX_UNIT_FRACTION_PARENT = 1,

        // Where the radix information is, telling where the decimal place
        // appears in the mantissa.  This give us 4 possible fixed point
        // representations as defined below.
        COMPLEX_RADIX_SHIFT = 4,
        COMPLEX_RADIX_MASK = 0x3,

        // The mantissa is an integral number -- i.e., 0xnnnnnn.0
        COMPLEX_RADIX_23p0 = 0,
        // The mantissa magnitude is 16 bits -- i.e, 0xnnnn.nn
        COMPLEX_RADIX_16p7 = 1,
        // The mantissa magnitude is 8 bits -- i.e, 0xnn.nnnn
        COMPLEX_RADIX_8p15 = 2,
        // The mantissa magnitude is 0 bits -- i.e, 0x0.nnnnnn
        COMPLEX_RADIX_0p23 = 3,

        // Where the actual value is.  This gives us 23 bits of
        // precision.  The top bit is the sign.
        COMPLEX_MANTISSA_SHIFT = 8,
        COMPLEX_MANTISSA_MASK = 0xffffff
    };

    // Possible data values for TYPE_NULL.
    enum {
        // The value is not defined.
        DATA_NULL_UNDEFINED = 0,
        // The value is explicitly defined as empty.
        DATA_NULL_EMPTY = 1
    };

    // The data for this item, as interpreted according to dataType.
    typedef uint32_t data_type;
    union {
      data_type data;
      float floatData;
    };

    void copyFrom_dtoh(const Res_value& src);
};

/**
 *  This is a reference to a unique entry (a ResTable_entry structure)
 *  in a resource table.  The value is structured as: 0xpptteeee,
 *  where pp is the package index, tt is the type index in that
 *  package, and eeee is the entry index in that type.  The package
 *  and type values start at 1 for the first item, to help catch cases
 *  where they have not been supplied.
 */
struct ResTable_ref
{
    uint32_t ident;
};

/**
 * Reference to a string in a string pool.
 */
struct ResStringPool_ref
{
    // Index into the string pool table (uint32_t-offset from the indices
    // immediately after ResStringPool_header) at which to find the location
    // of the string data in the pool.
    uint32_t index;
};

/** ********************************************************************
 *  String Pool
 *
 *  A set of strings that can be references by others through a
 *  ResStringPool_ref.
 *
 *********************************************************************** */

/**
 * Definition for a pool of strings.  The data of this chunk is an
 * array of uint32_t providing indices into the pool, relative to
 * stringsStart.  At stringsStart are all of the UTF-16 strings
 * concatenated together; each starts with a uint16_t of the string's
 * length and each ends with a 0x0000 terminator.  If a string is >
 * 32767 characters, the high bit of the length is set meaning to take
 * those 15 bits as a high word and it will be followed by another
 * uint16_t containing the low word.
 *
 * If styleCount is not zero, then immediately following the array of
 * uint32_t indices into the string table is another array of indices
 * into a style table starting at stylesStart.  Each entry in the
 * style table is an array of ResStringPool_span structures.
 */
struct ResStringPool_header
{
    struct ResChunk_header header;

    // Number of strings in this pool (number of uint32_t indices that follow
    // in the data).
    uint32_t stringCount;

    // Number of style span arrays in the pool (number of uint32_t indices
    // follow the string indices).
    uint32_t styleCount;

    // Flags.
    enum {
        // If set, the string index is sorted by the string values (based
        // on strcmp16()).
        SORTED_FLAG = 1<<0,

        // String pool is encoded in UTF-8
        UTF8_FLAG = 1<<8
    };
    uint32_t flags;

    // Index from header of the string data.
    uint32_t stringsStart;

    // Index from header of the style data.
    uint32_t stylesStart;
};

/**
 * This structure defines a span of style information associated with
 * a string in the pool.
 */
struct ResStringPool_span
{
    enum : uint32_t {
        END = 0xFFFFFFFF
    };

    // This is the name of the span -- that is, the name of the XML
    // tag that defined it.  The special value END (0xFFFFFFFF) indicates
    // the end of an array of spans.
    ResStringPool_ref name;

    // The range of characters in the string that this span applies to.
    uint32_t firstChar, lastChar;
};

/**
 * Convenience class for accessing data in a ResStringPool resource.
 */
class ResStringPool
{
public:
    ResStringPool();
    ResStringPool(const void* data, size_t size, bool copyData=false);
    ~ResStringPool();

    void setToEmpty();
    status_t setTo(const void* data, size_t size, bool copyData=true);

    status_t getError() const;

    void uninit();

    // Return string entry as UTF16; if the pool is UTF8, the string will
    // be converted before returning.
    inline const char16_t* stringAt(const ResStringPool_ref& ref, size_t* outLen) const {
        return stringAt(ref.index, outLen);
    }
    const char16_t* stringAt(size_t idx, size_t* outLen) const;

    // Note: returns null if the string pool is not UTF8.
    const char* string8At(size_t idx, size_t* outLen) const;

    // Return string whether the pool is UTF8 or UTF16.  Does not allow you
    // to distinguish null.
    const String8 string8ObjectAt(size_t idx) const;

    const ResStringPool_span* styleAt(const ResStringPool_ref& ref) const;
    const ResStringPool_span* styleAt(size_t idx) const;

    ssize_t indexOfString(const char16_t* str, size_t strLen) const;

    size_t size() const;
    size_t styleCount() const;
    size_t bytes() const;

    bool isSorted() const;
    bool isUTF8() const;

private:
    status_t                    mError;
    void*                       mOwnedData;
    const ResStringPool_header* mHeader;
    size_t                      mSize;
#if !defined(_MSC_VER) && !defined(__MINGW64__) && !defined(__MINGW32__)
    mutable Mutex               mDecodeLock;
#else
    mutable std::mutex          mDecodeLock;
#endif
    const uint32_t*             mEntries;
    const uint32_t*             mEntryStyles;
    const void*                 mStrings;
    char16_t mutable**          mCache;
    uint32_t                    mStringPoolSize;    // number of uint16_t
    const uint32_t*             mStyles;
    uint32_t                    mStylePoolSize;    // number of uint32_t
};

/** ********************************************************************
 *  XML Tree
 *
 *  Binary representation of an XML document.  This is designed to
 *  express everything in an XML document, in a form that is much
 *  easier to parse on the device.
 *
 *********************************************************************** */

/**
 * XML tree header.  This appears at the front of an XML tree,
 * describing its content.  It is followed by a flat array of
 * ResXMLTree_node structures; the hierarchy of the XML document
 * is described by the occurrance of RES_XML_START_ELEMENT_TYPE
 * and corresponding RES_XML_END_ELEMENT_TYPE nodes in the array.
 */
struct ResXMLTree_header
{
    struct ResChunk_header header;
};

/**
 * Basic XML tree node.  A single item in the XML document.  Extended info
 * about the node can be found after header.headerSize.
 */
struct ResXMLTree_node
{
    struct ResChunk_header header;

    // Line number in original source file at which this element appeared.
    uint32_t lineNumber;

    // Optional XML comment that was associated with this element; -1 if none.
    struct ResStringPool_ref comment;
};

/**
 * Extended XML tree node for CDATA tags -- includes the CDATA string.
 * Appears header.headerSize bytes after a ResXMLTree_node.
 */
struct ResXMLTree_cdataExt
{
    // The raw CDATA character data.
    struct ResStringPool_ref data;

    // The typed value of the character data if this is a CDATA node.
    struct Res_value typedData;
};

/**
 * Extended XML tree node for namespace start/end nodes.
 * Appears header.headerSize bytes after a ResXMLTree_node.
 */
struct ResXMLTree_namespaceExt
{
    // The prefix of the namespace.
    struct ResStringPool_ref prefix;

    // The URI of the namespace.
    struct ResStringPool_ref uri;
};

/**
 * Extended XML tree node for element start/end nodes.
 * Appears header.headerSize bytes after a ResXMLTree_node.
 */
struct ResXMLTree_endElementExt
{
    // String of the full namespace of this element.
    struct ResStringPool_ref ns;

    // String name of this node if it is an ELEMENT; the raw
    // character data if this is a CDATA node.
    struct ResStringPool_ref name;
};

/**
 * Extended XML tree node for start tags -- includes attribute
 * information.
 * Appears header.headerSize bytes after a ResXMLTree_node.
 */
struct ResXMLTree_attrExt
{
    // String of the full namespace of this element.
    struct ResStringPool_ref ns;

    // String name of this node if it is an ELEMENT; the raw
    // character data if this is a CDATA node.
    struct ResStringPool_ref name;

    // Byte offset from the start of this structure where the attributes start.
    uint16_t attributeStart;

    // Size of the ResXMLTree_attribute structures that follow.
    uint16_t attributeSize;

    // Number of attributes associated with an ELEMENT.  These are
    // available as an array of ResXMLTree_attribute structures
    // immediately following this node.
    uint16_t attributeCount;

    // Index (1-based) of the "id" attribute. 0 if none.
    uint16_t idIndex;

    // Index (1-based) of the "class" attribute. 0 if none.
    uint16_t classIndex;

    // Index (1-based) of the "style" attribute. 0 if none.
    uint16_t styleIndex;
};

struct ResXMLTree_attribute
{
    // Namespace of this attribute.
    struct ResStringPool_ref ns;

    // Name of this attribute.
    struct ResStringPool_ref name;

    // The original raw string value of this attribute.
    struct ResStringPool_ref rawValue;

    // Processesd typed value of this attribute.
    struct Res_value typedValue;
};

class ResXMLTree;

class ResXMLParser
{
public:
    ResXMLParser(const ResXMLTree& tree);

    enum event_code_t {
        BAD_DOCUMENT = -1,
        START_DOCUMENT = 0,
        END_DOCUMENT = 1,

        FIRST_CHUNK_CODE = RES_XML_FIRST_CHUNK_TYPE,

        START_NAMESPACE = RES_XML_START_NAMESPACE_TYPE,
        END_NAMESPACE = RES_XML_END_NAMESPACE_TYPE,
        START_TAG = RES_XML_START_ELEMENT_TYPE,
        END_TAG = RES_XML_END_ELEMENT_TYPE,
        TEXT = RES_XML_CDATA_TYPE
    };

    struct ResXMLPosition
    {
        event_code_t                eventCode;
        const ResXMLTree_node*      curNode;
        const void*                 curExt;
    };

    void restart();

    const ResStringPool& getStrings() const;

    event_code_t getEventType() const;
    // Note, unlike XmlPullParser, the first call to next() will return
    // START_TAG of the first element.
    event_code_t next();

    // These are available for all nodes:
    int32_t getCommentID() const;
    const char16_t* getComment(size_t* outLen) const;
    uint32_t getLineNumber() const;

    // This is available for TEXT:
    int32_t getTextID() const;
    const char16_t* getText(size_t* outLen) const;
    ssize_t getTextValue(Res_value* outValue) const;

    // These are available for START_NAMESPACE and END_NAMESPACE:
    int32_t getNamespacePrefixID() const;
    const char16_t* getNamespacePrefix(size_t* outLen) const;
    int32_t getNamespaceUriID() const;
    const char16_t* getNamespaceUri(size_t* outLen) const;

    // These are available for START_TAG and END_TAG:
    int32_t getElementNamespaceID() const;
    const char16_t* getElementNamespace(size_t* outLen) const;
    int32_t getElementNameID() const;
    const char16_t* getElementName(size_t* outLen) const;

    // Remaining methods are for retrieving information about attributes
    // associated with a START_TAG:

    size_t getAttributeCount() const;

    // Returns -1 if no namespace, -2 if idx out of range.
    int32_t getAttributeNamespaceID(size_t idx) const;
    const char16_t* getAttributeNamespace(size_t idx, size_t* outLen) const;

    int32_t getAttributeNameID(size_t idx) const;
    const char16_t* getAttributeName(size_t idx, size_t* outLen) const;
    uint32_t getAttributeNameResID(size_t idx) const;

    // These will work only if the underlying string pool is UTF-8.
    const char* getAttributeNamespace8(size_t idx, size_t* outLen) const;
    const char* getAttributeName8(size_t idx, size_t* outLen) const;

    int32_t getAttributeValueStringID(size_t idx) const;
    const char16_t* getAttributeStringValue(size_t idx, size_t* outLen) const;

    int32_t getAttributeDataType(size_t idx) const;
    int32_t getAttributeData(size_t idx) const;

    ssize_t getAttributeValue(size_t idx, Res_value* outValue) const;

    ssize_t indexOfAttribute(const char* ns, const char* attr) const;
    ssize_t indexOfAttribute(const char16_t* ns, size_t nsLen,
                             const char16_t* attr, size_t attrLen) const;

    ssize_t indexOfID() const;
    ssize_t indexOfClass() const;
    ssize_t indexOfStyle() const;

    void getPosition(ResXMLPosition* pos) const;
    void setPosition(const ResXMLPosition& pos);

private:
    friend class ResXMLTree;

    event_code_t nextNode();

    ResXMLTree_attribute* getAttributePointer(size_t idx) const;

    const ResXMLTree&           mTree;
    event_code_t                mEventCode;
    const ResXMLTree_node*      mCurNode;
    const void*                 mCurExt;
};

class DynamicRefTable;

/**
 * Convenience class for accessing data in a ResXMLTree resource.
 */
class ResXMLTree : public ResXMLParser
{
public:
    ResXMLTree(const DynamicRefTable* dynamicRefTable);
    ResXMLTree();
    ~ResXMLTree();

    status_t setTo(const void* data, size_t size, bool copyData=false);

    status_t getError() const;

    void uninit();

private:
    friend class ResXMLParser;

    status_t validateNode(const ResXMLTree_node* node) const;

    const DynamicRefTable* const mDynamicRefTable;

    status_t                    mError;
    void*                       mOwnedData;
    const ResXMLTree_header*    mHeader;
    size_t                      mSize;
    const uint8_t*              mDataEnd;
    ResStringPool               mStrings;
    uint32_t*                   mResIds;
    size_t                      mNumResIds;
    const ResXMLTree_node*      mRootNode;
    const void*                 mRootExt;
    event_code_t                mRootCode;
};

/** ********************************************************************
 *  RESOURCE TABLE
 *
 *********************************************************************** */

/**
 * Header for a resource table.  Its data contains a series of
 * additional chunks:
 *   * A ResStringPool_header containing all table values.  This string pool
 *     contains all of the string values in the entire resource table (not
 *     the names of entries or type identifiers however).
 *   * One or more ResTable_package chunks.
 *
 * Specific entries within a resource table can be uniquely identified
 * with a single integer as defined by the ResTable_ref structure.
 */
struct ResTable_header
{
    struct ResChunk_header header;

    // The number of ResTable_package structures.
    uint32_t packageCount;
};

/**
 * A collection of resource data types within a package.  Followed by
 * one or more ResTable_type and ResTable_typeSpec structures containing the
 * entry values for each resource type.
 */
struct ResTable_package
{
    struct ResChunk_header header;

    // If this is a base package, its ID.  Package IDs start
    // at 1 (corresponding to the value of the package bits in a
    // resource identifier).  0 means this is not a base package.
    uint32_t id;

    // Actual name of this package, \0-terminated.
    uint16_t name[128];

    // Offset to a ResStringPool_header defining the resource
    // type symbol table.  If zero, this package is inheriting from
    // another base package (overriding specific values in it).
    uint32_t typeStrings;

    // Last index into typeStrings that is for public use by others.
    uint32_t lastPublicType;

    // Offset to a ResStringPool_header defining the resource
    // key symbol table.  If zero, this package is inheriting from
    // another base package (overriding specific values in it).
    uint32_t keyStrings;

    // Last index into keyStrings that is for public use by others.
    uint32_t lastPublicKey;

    uint32_t typeIdOffset;
};

// The most specific locale can consist of:
//
// - a 3 char language code
// - a 3 char region code prefixed by a 'r'
// - a 4 char script code prefixed by a 's'
// - a 8 char variant code prefixed by a 'v'
//
// each separated by a single char separator, which sums up to a total of 24
// chars, (25 include the string terminator) rounded up to 28 to be 4 byte
// aligned.
#define RESTABLE_MAX_LOCALE_LEN 28


/**
 * Describes a particular resource configuration.
 */
struct ResTable_config
{
    // Number of bytes in this structure.
    uint32_t size;

    union {
        struct {
            // Mobile country code (from SIM).  0 means "any".
            uint16_t mcc;
            // Mobile network code (from SIM).  0 means "any".
            uint16_t mnc;
        };
        uint32_t imsi;
    };

    union {
        struct {
            // This field can take three different forms:
            // - \0\0 means "any".
            //
            // - Two 7 bit ascii values interpreted as ISO-639-1 language
            //   codes ('fr', 'en' etc. etc.). The high bit for both bytes is
            //   zero.
            //
            // - A single 16 bit little endian packed value representing an
            //   ISO-639-2 3 letter language code. This will be of the form:
            //
            //   {1, t, t, t, t, t, s, s, s, s, s, f, f, f, f, f}
            //
            //   bit[0, 4] = first letter of the language code
            //   bit[5, 9] = second letter of the language code
            //   bit[10, 14] = third letter of the language code.
            //   bit[15] = 1 always
            //
            // For backwards compatibility, languages that have unambiguous
            // two letter codes are represented in that format.
            //
            // The layout is always bigendian irrespective of the runtime
            // architecture.
            char language[2];

            // This field can take three different forms:
            // - \0\0 means "any".
            //
            // - Two 7 bit ascii values interpreted as 2 letter region
            //   codes ('US', 'GB' etc.). The high bit for both bytes is zero.
            //
            // - An UN M.49 3 digit region code. For simplicity, these are packed
            //   in the same manner as the language codes, though we should need
            //   only 10 bits to represent them, instead of the 15.
            //
            // The layout is always bigendian irrespective of the runtime
            // architecture.
            char country[2];
        };
        uint32_t locale;
    };

    enum {
        ORIENTATION_ANY  = ACONFIGURATION_ORIENTATION_ANY,
        ORIENTATION_PORT = ACONFIGURATION_ORIENTATION_PORT,
        ORIENTATION_LAND = ACONFIGURATION_ORIENTATION_LAND,
        ORIENTATION_SQUARE = ACONFIGURATION_ORIENTATION_SQUARE,
    };

    enum {
        TOUCHSCREEN_ANY  = ACONFIGURATION_TOUCHSCREEN_ANY,
        TOUCHSCREEN_NOTOUCH  = ACONFIGURATION_TOUCHSCREEN_NOTOUCH,
        TOUCHSCREEN_STYLUS  = ACONFIGURATION_TOUCHSCREEN_STYLUS,
        TOUCHSCREEN_FINGER  = ACONFIGURATION_TOUCHSCREEN_FINGER,
    };

    enum {
        DENSITY_DEFAULT = ACONFIGURATION_DENSITY_DEFAULT,
        DENSITY_LOW = ACONFIGURATION_DENSITY_LOW,
        DENSITY_MEDIUM = ACONFIGURATION_DENSITY_MEDIUM,
        DENSITY_TV = ACONFIGURATION_DENSITY_TV,
        DENSITY_HIGH = ACONFIGURATION_DENSITY_HIGH,
        DENSITY_XHIGH = ACONFIGURATION_DENSITY_XHIGH,
        DENSITY_XXHIGH = ACONFIGURATION_DENSITY_XXHIGH,
        DENSITY_XXXHIGH = ACONFIGURATION_DENSITY_XXXHIGH,
        DENSITY_ANY = ACONFIGURATION_DENSITY_ANY,
        DENSITY_NONE = ACONFIGURATION_DENSITY_NONE
    };

    union {
        struct {
            uint8_t orientation;
            uint8_t touchscreen;
            uint16_t density;
        };
        uint32_t screenType;
    };

    enum {
        KEYBOARD_ANY  = ACONFIGURATION_KEYBOARD_ANY,
        KEYBOARD_NOKEYS  = ACONFIGURATION_KEYBOARD_NOKEYS,
        KEYBOARD_QWERTY  = ACONFIGURATION_KEYBOARD_QWERTY,
        KEYBOARD_12KEY  = ACONFIGURATION_KEYBOARD_12KEY,
    };

    enum {
        NAVIGATION_ANY  = ACONFIGURATION_NAVIGATION_ANY,
        NAVIGATION_NONAV  = ACONFIGURATION_NAVIGATION_NONAV,
        NAVIGATION_DPAD  = ACONFIGURATION_NAVIGATION_DPAD,
        NAVIGATION_TRACKBALL  = ACONFIGURATION_NAVIGATION_TRACKBALL,
        NAVIGATION_WHEEL  = ACONFIGURATION_NAVIGATION_WHEEL,
    };

    enum {
        MASK_KEYSHIDDEN = 0x0003,
        KEYSHIDDEN_ANY = ACONFIGURATION_KEYSHIDDEN_ANY,
        KEYSHIDDEN_NO = ACONFIGURATION_KEYSHIDDEN_NO,
        KEYSHIDDEN_YES = ACONFIGURATION_KEYSHIDDEN_YES,
        KEYSHIDDEN_SOFT = ACONFIGURATION_KEYSHIDDEN_SOFT,
    };

    enum {
        MASK_NAVHIDDEN = 0x000c,
        SHIFT_NAVHIDDEN = 2,
        NAVHIDDEN_ANY = ACONFIGURATION_NAVHIDDEN_ANY << SHIFT_NAVHIDDEN,
        NAVHIDDEN_NO = ACONFIGURATION_NAVHIDDEN_NO << SHIFT_NAVHIDDEN,
        NAVHIDDEN_YES = ACONFIGURATION_NAVHIDDEN_YES << SHIFT_NAVHIDDEN,
    };

    union {
        struct {
            uint8_t keyboard;
            uint8_t navigation;
            uint8_t inputFlags;
            uint8_t inputPad0;
        };
        uint32_t input;
    };

    enum {
        SCREENWIDTH_ANY = 0
    };

    enum {
        SCREENHEIGHT_ANY = 0
    };

    union {
        struct {
            uint16_t screenWidth;
            uint16_t screenHeight;
        };
        uint32_t screenSize;
    };

    enum {
        SDKVERSION_ANY = 0
    };

    enum {
        MINORVERSION_ANY = 0
    };

    union {
        struct {
            uint16_t sdkVersion;
            // For now minorVersion must always be 0!!!  Its meaning
            // is currently undefined.
            uint16_t minorVersion;
        };
        uint32_t version;
    };

    enum {
        // screenLayout bits for screen size class.
        MASK_SCREENSIZE = 0x0f,
        SCREENSIZE_ANY = ACONFIGURATION_SCREENSIZE_ANY,
        SCREENSIZE_SMALL = ACONFIGURATION_SCREENSIZE_SMALL,
        SCREENSIZE_NORMAL = ACONFIGURATION_SCREENSIZE_NORMAL,
        SCREENSIZE_LARGE = ACONFIGURATION_SCREENSIZE_LARGE,
        SCREENSIZE_XLARGE = ACONFIGURATION_SCREENSIZE_XLARGE,

        // screenLayout bits for wide/long screen variation.
        MASK_SCREENLONG = 0x30,
        SHIFT_SCREENLONG = 4,
        SCREENLONG_ANY = ACONFIGURATION_SCREENLONG_ANY << SHIFT_SCREENLONG,
        SCREENLONG_NO = ACONFIGURATION_SCREENLONG_NO << SHIFT_SCREENLONG,
        SCREENLONG_YES = ACONFIGURATION_SCREENLONG_YES << SHIFT_SCREENLONG,

        // screenLayout bits for layout direction.
        MASK_LAYOUTDIR = 0xC0,
        SHIFT_LAYOUTDIR = 6,
        LAYOUTDIR_ANY = ACONFIGURATION_LAYOUTDIR_ANY << SHIFT_LAYOUTDIR,
        LAYOUTDIR_LTR = ACONFIGURATION_LAYOUTDIR_LTR << SHIFT_LAYOUTDIR,
        LAYOUTDIR_RTL = ACONFIGURATION_LAYOUTDIR_RTL << SHIFT_LAYOUTDIR,
    };

    enum {
        // uiMode bits for the mode type.
        MASK_UI_MODE_TYPE = 0x0f,
        UI_MODE_TYPE_ANY = ACONFIGURATION_UI_MODE_TYPE_ANY,
        UI_MODE_TYPE_NORMAL = ACONFIGURATION_UI_MODE_TYPE_NORMAL,
        UI_MODE_TYPE_DESK = ACONFIGURATION_UI_MODE_TYPE_DESK,
        UI_MODE_TYPE_CAR = ACONFIGURATION_UI_MODE_TYPE_CAR,
        UI_MODE_TYPE_TELEVISION = ACONFIGURATION_UI_MODE_TYPE_TELEVISION,
        UI_MODE_TYPE_APPLIANCE = ACONFIGURATION_UI_MODE_TYPE_APPLIANCE,
        UI_MODE_TYPE_WATCH = ACONFIGURATION_UI_MODE_TYPE_WATCH,
        UI_MODE_TYPE_VR_HEADSET = ACONFIGURATION_UI_MODE_TYPE_VR_HEADSET,

        // uiMode bits for the night switch.
        MASK_UI_MODE_NIGHT = 0x30,
        SHIFT_UI_MODE_NIGHT = 4,
        UI_MODE_NIGHT_ANY = ACONFIGURATION_UI_MODE_NIGHT_ANY << SHIFT_UI_MODE_NIGHT,
        UI_MODE_NIGHT_NO = ACONFIGURATION_UI_MODE_NIGHT_NO << SHIFT_UI_MODE_NIGHT,
        UI_MODE_NIGHT_YES = ACONFIGURATION_UI_MODE_NIGHT_YES << SHIFT_UI_MODE_NIGHT,
    };

    union {
        struct {
            uint8_t screenLayout;
            uint8_t uiMode;
            uint16_t smallestScreenWidthDp;
        };
        uint32_t screenConfig;
    };

    union {
        struct {
            uint16_t screenWidthDp;
            uint16_t screenHeightDp;
        };
        uint32_t screenSizeDp;
    };

    // The ISO-15924 short name for the script corresponding to this
    // configuration. (eg. Hant, Latn, etc.). Interpreted in conjunction with
    // the locale field.
    char localeScript[4];

    // A single BCP-47 variant subtag. Will vary in length between 5 and 8
    // chars. Interpreted in conjunction with the locale field.
    char localeVariant[8];

    enum {
        // screenLayout2 bits for round/notround.
        MASK_SCREENROUND = 0x03,
        SCREENROUND_ANY = ACONFIGURATION_SCREENROUND_ANY,
        SCREENROUND_NO = ACONFIGURATION_SCREENROUND_NO,
        SCREENROUND_YES = ACONFIGURATION_SCREENROUND_YES,
    };

    enum {
        // colorMode bits for wide-color gamut/narrow-color gamut.
        MASK_WIDE_COLOR_GAMUT = 0x03,
        WIDE_COLOR_GAMUT_ANY = ACONFIGURATION_WIDE_COLOR_GAMUT_ANY,
        WIDE_COLOR_GAMUT_NO = ACONFIGURATION_WIDE_COLOR_GAMUT_NO,
        WIDE_COLOR_GAMUT_YES = ACONFIGURATION_WIDE_COLOR_GAMUT_YES,

        // colorMode bits for HDR/LDR.
        MASK_HDR = 0x0c,
        SHIFT_COLOR_MODE_HDR = 2,
        HDR_ANY = ACONFIGURATION_HDR_ANY << SHIFT_COLOR_MODE_HDR,
        HDR_NO = ACONFIGURATION_HDR_NO << SHIFT_COLOR_MODE_HDR,
        HDR_YES = ACONFIGURATION_HDR_YES << SHIFT_COLOR_MODE_HDR,
    };

    // An extension of screenConfig.
    union {
        struct {
            uint8_t screenLayout2;      // Contains round/notround qualifier.
            uint8_t colorMode;          // Wide-gamut, HDR, etc.
            uint16_t screenConfigPad2;  // Reserved padding.
        };
        uint32_t screenConfig2;
    };
    // If false and localeScript is set, it means that the script of the locale
    // was explicitly provided.
    //
    // If true, it means that localeScript was automatically computed.
    // localeScript may still not be set in this case, which means that we
    // tried but could not compute a script.
    bool localeScriptWasComputed;

    // The value of BCP 47 Unicode extension for key 'nu' (numbering system).
    // Varies in length from 3 to 8 chars. Zero-filled value.
    char localeNumberingSystem[8];

    void copyFromDeviceNoSwap(const ResTable_config& o);

    void copyFromDtoH(const ResTable_config& o);

    void swapHtoD();

    int compare(const ResTable_config& o) const;
    int compareLogical(const ResTable_config& o) const;

    // Flags indicating a set of config values.  These flag constants must
    // match the corresponding ones in android.content.pm.ActivityInfo and
    // attrs_manifest.xml.
    enum {
        CONFIG_MCC = ACONFIGURATION_MCC,
        CONFIG_MNC = ACONFIGURATION_MNC,
        CONFIG_LOCALE = ACONFIGURATION_LOCALE,
        CONFIG_TOUCHSCREEN = ACONFIGURATION_TOUCHSCREEN,
        CONFIG_KEYBOARD = ACONFIGURATION_KEYBOARD,
        CONFIG_KEYBOARD_HIDDEN = ACONFIGURATION_KEYBOARD_HIDDEN,
        CONFIG_NAVIGATION = ACONFIGURATION_NAVIGATION,
        CONFIG_ORIENTATION = ACONFIGURATION_ORIENTATION,
        CONFIG_DENSITY = ACONFIGURATION_DENSITY,
        CONFIG_SCREEN_SIZE = ACONFIGURATION_SCREEN_SIZE,
        CONFIG_SMALLEST_SCREEN_SIZE = ACONFIGURATION_SMALLEST_SCREEN_SIZE,
        CONFIG_VERSION = ACONFIGURATION_VERSION,
        CONFIG_SCREEN_LAYOUT = ACONFIGURATION_SCREEN_LAYOUT,
        CONFIG_UI_MODE = ACONFIGURATION_UI_MODE,
        CONFIG_LAYOUTDIR = ACONFIGURATION_LAYOUTDIR,
        CONFIG_SCREEN_ROUND = ACONFIGURATION_SCREEN_ROUND,
        CONFIG_COLOR_MODE = ACONFIGURATION_COLOR_MODE,
    };

    // Compare two configuration, returning CONFIG_* flags set for each value
    // that is different.
    int diff(const ResTable_config& o) const;

    // Return true if 'this' is more specific than 'o'.
    bool isMoreSpecificThan(const ResTable_config& o) const;

    // Return true if 'this' is a better match than 'o' for the 'requested'
    // configuration.  This assumes that match() has already been used to
    // remove any configurations that don't match the requested configuration
    // at all; if they are not first filtered, non-matching results can be
    // considered better than matching ones.
    // The general rule per attribute: if the request cares about an attribute
    // (it normally does), if the two (this and o) are equal it's a tie.  If
    // they are not equal then one must be generic because only generic and
    // '==requested' will pass the match() call.  So if this is not generic,
    // it wins.  If this IS generic, o wins (return false).
    bool isBetterThan(const ResTable_config& o, const ResTable_config* requested) const;

    // Return true if 'this' can be considered a match for the parameters in
    // 'settings'.
    // Note this is asymetric.  A default piece of data will match every request
    // but a request for the default should not match odd specifics
    // (ie, request with no mcc should not match a particular mcc's data)
    // settings is the requested settings
    bool match(const ResTable_config& settings) const;

    // Get the string representation of the locale component of this
    // Config. The maximum size of this representation will be
    // |RESTABLE_MAX_LOCALE_LEN| (including a terminating '\0').
    //
    // Example: en-US, en-Latn-US, en-POSIX.
    void getBcp47Locale(char* out) const;

    // Sets the values of language, region, script and variant to the
    // well formed BCP-47 locale contained in |in|. The input locale is
    // assumed to be valid and no validation is performed.
    void setBcp47Locale(const char* in);

    inline void clearLocale() {
        locale = 0;
        memset(localeScript, 0, sizeof(localeScript));
        memset(localeVariant, 0, sizeof(localeVariant));
    }

    // Get the 2 or 3 letter language code of this configuration. Trailing
    // bytes are set to '\0'.
    size_t unpackLanguage(char language[4]) const;
    // Get the 2 or 3 letter language code of this configuration. Trailing
    // bytes are set to '\0'.
    size_t unpackRegion(char region[4]) const;

    // Sets the language code of this configuration to the first three
    // chars at |language|.
    //
    // If |language| is a 2 letter code, the trailing byte must be '\0' or
    // the BCP-47 separator '-'.
    void packLanguage(const char* language);
    // Sets the region code of this configuration to the first three bytes
    // at |region|. If |region| is a 2 letter code, the trailing byte must be '\0'
    // or the BCP-47 separator '-'.
    void packRegion(const char* region);

    // Returns a positive integer if this config is more specific than |o|
    // with respect to their locales, a negative integer if |o| is more specific
    // and 0 if they're equally specific.
    int isLocaleMoreSpecificThan(const ResTable_config &o) const;

    String8 toString() const;
};

/**
 * A specification of the resources defined by a particular type.
 *
 * There should be one of these chunks for each resource type.
 *
 * This structure is followed by an array of integers providing the set of
 * configuration change flags (ResTable_config::CONFIG_*) that have multiple
 * resources for that configuration.  In addition, the high bit is set if that
 * resource has been made public.
 */
struct ResTable_typeSpec
{
    struct ResChunk_header header;

    // The type identifier this chunk is holding.  Type IDs start
    // at 1 (corresponding to the value of the type bits in a
    // resource identifier).  0 is invalid.
    uint8_t id;

    // Must be 0.
    uint8_t res0;
    // Must be 0.
    uint16_t res1;

    // Number of uint32_t entry configuration masks that follow.
    uint32_t entryCount;

    enum {
        // Additional flag indicating an entry is public.
        SPEC_PUBLIC = 0x40000000
    };
};

/**
 * A collection of resource entries for a particular resource data
 * type. Followed by an array of uint32_t defining the resource
 * values, corresponding to the array of type strings in the
 * ResTable_package::typeStrings string block. Each of these hold an
 * index from entriesStart; a value of NO_ENTRY means that entry is
 * not defined.
 *
 * There may be multiple of these chunks for a particular resource type,
 * supply different configuration variations for the resource values of
 * that type.
 *
 * It would be nice to have an additional ordered index of entries, so
 * we can do a binary search if trying to find a resource by string name.
 */
struct ResTable_type
{
    struct ResChunk_header header;

    enum : uint32_t {
        NO_ENTRY = 0xFFFFFFFF
    };

    // The type identifier this chunk is holding.  Type IDs start
    // at 1 (corresponding to the value of the type bits in a
    // resource identifier).  0 is invalid.
    uint8_t id;

    // Must be 0.
    uint8_t res0;
    // Must be 0.
    uint16_t res1;

    // Number of uint32_t entry indices that follow.
    uint32_t entryCount;

    // Offset from header where ResTable_entry data starts.
    uint32_t entriesStart;

    // Configuration this collection of entries is designed for.
    ResTable_config config;
};

/**
 * This is the beginning of information about an entry in the resource
 * table.  It holds the reference to the name of this entry, and is
 * immediately followed by one of:
 *   * A Res_value structure, if FLAG_COMPLEX is -not- set.
 *   * An array of ResTable_map structures, if FLAG_COMPLEX is set.
 *     These supply a set of name/value mappings of data.
 */
struct ResTable_entry
{
    // Number of bytes in this structure.
    uint16_t size;

    enum {
        // If set, this is a complex entry, holding a set of name/value
        // mappings.  It is followed by an array of ResTable_map structures.
        FLAG_COMPLEX = 0x0001,
        // If set, this resource has been declared public, so libraries
        // are allowed to reference it.
        FLAG_PUBLIC = 0x0002
    };
    uint16_t flags;

    // Reference into ResTable_package::keyStrings identifying this entry.
    struct ResStringPool_ref key;
};

/**
 * Extended form of a ResTable_entry for map entries, defining a parent map
 * resource from which to inherit values.
 */
struct ResTable_map_entry : public ResTable_entry
{
    // Resource identifier of the parent mapping, or 0 if there is none.
    // This is always treated as a TYPE_DYNAMIC_REFERENCE.
    ResTable_ref parent;
    // Number of name/value pairs that follow for FLAG_COMPLEX.
    uint32_t count;
};

/**
 * A single name/value mapping that is part of a complex resource
 * entry.
 */
struct ResTable_map
{
    // The resource identifier defining this mapping's name.  For attribute
    // resources, 'name' can be one of the following special resource types
    // to supply meta-data about the attribute; for all other resource types
    // it must be an attribute resource.
    ResTable_ref name;

    // Special values for 'name' when defining attribute resources.
    enum {
        // This entry holds the attribute's type code.
        ATTR_TYPE = Res_MAKEINTERNAL(0),

        // For integral attributes, this is the minimum value it can hold.
        ATTR_MIN = Res_MAKEINTERNAL(1),

        // For integral attributes, this is the maximum value it can hold.
        ATTR_MAX = Res_MAKEINTERNAL(2),

        // Localization of this resource is can be encouraged or required with
        // an aapt flag if this is set
        ATTR_L10N = Res_MAKEINTERNAL(3),

        // for plural support, see android.content.res.PluralRules#attrForQuantity(int)
        ATTR_OTHER = Res_MAKEINTERNAL(4),
        ATTR_ZERO = Res_MAKEINTERNAL(5),
        ATTR_ONE = Res_MAKEINTERNAL(6),
        ATTR_TWO = Res_MAKEINTERNAL(7),
        ATTR_FEW = Res_MAKEINTERNAL(8),
        ATTR_MANY = Res_MAKEINTERNAL(9)

    };

    // Bit mask of allowed types, for use with ATTR_TYPE.
    enum {
        // No type has been defined for this attribute, use generic
        // type handling.  The low 16 bits are for types that can be
        // handled generically; the upper 16 require additional information
        // in the bag so can not be handled generically for TYPE_ANY.
        TYPE_ANY = 0x0000FFFF,

        // Attribute holds a references to another resource.
        TYPE_REFERENCE = 1<<0,

        // Attribute holds a generic string.
        TYPE_STRING = 1<<1,

        // Attribute holds an integer value.  ATTR_MIN and ATTR_MIN can
        // optionally specify a constrained range of possible integer values.
        TYPE_INTEGER = 1<<2,

        // Attribute holds a boolean integer.
        TYPE_BOOLEAN = 1<<3,

        // Attribute holds a color value.
        TYPE_COLOR = 1<<4,

        // Attribute holds a floating point value.
        TYPE_FLOAT = 1<<5,

        // Attribute holds a dimension value, such as "20px".
        TYPE_DIMENSION = 1<<6,

        // Attribute holds a fraction value, such as "20%".
        TYPE_FRACTION = 1<<7,

        // Attribute holds an enumeration.  The enumeration values are
        // supplied as additional entries in the map.
        TYPE_ENUM = 1<<16,

        // Attribute holds a bitmaks of flags.  The flag bit values are
        // supplied as additional entries in the map.
        TYPE_FLAGS = 1<<17
    };

    // Enum of localization modes, for use with ATTR_L10N.
    enum {
        L10N_NOT_REQUIRED = 0,
        L10N_SUGGESTED    = 1
    };

    // This mapping's value.
    Res_value value;
};

/**
 * A package-id to package name mapping for any shared libraries used
 * in this resource table. The package-id's encoded in this resource
 * table may be different than the id's assigned at runtime. We must
 * be able to translate the package-id's based on the package name.
 */
struct ResTable_lib_header
{
    struct ResChunk_header header;

    // The number of shared libraries linked in this resource table.
    uint32_t count;
};

/**
 * A shared library package-id to package name entry.
 */
struct ResTable_lib_entry
{
    // The package-id this shared library was assigned at build time.
    // We use a uint32 to keep the structure aligned on a uint32 boundary.
    uint32_t packageId;

    // The package name of the shared library. \0 terminated.
    uint16_t packageName[128];
};

/**
 * Holds the shared library ID table. Shared libraries are assigned package IDs at
 * build time, but they may be loaded in a different order, so we need to maintain
 * a mapping of build-time package ID to run-time assigned package ID.
 *
 * Dynamic references are not currently supported in overlays. Only the base package
 * may have dynamic references.
 */
class DynamicRefTable
{
public:
    DynamicRefTable(uint8_t packageId);

    // Loads an unmapped reference table from the package.
    status_t load(const ResTable_lib_header* const header);

    // Adds mappings from the other DynamicRefTable
    status_t addMappings(const DynamicRefTable& other);

    // Creates a mapping from build-time package ID to run-time package ID for
    // the given package.
    status_t addMapping(const String16& packageName, uint8_t packageId);

    // Performs the actual conversion of build-time resource ID to run-time
    // resource ID.
    inline status_t lookupResourceId(uint32_t* resId) const;
    inline status_t lookupResourceValue(Res_value* value) const;

    inline const KeyedVector<String16, uint8_t>& entries() const {
        return mEntries;
    }

private:
    const uint8_t                   mAssignedPackageId;
    uint8_t                         mLookupTable[256];
    KeyedVector<String16, uint8_t>  mEntries;
};

bool U16StringToInt(const char16_t* s, size_t len, Res_value* outValue);

}   // namespace android

#endif // _LIBS_UTILS_RESOURCE_TYPES_H
