/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <assert.h>
#include <string>

#include "Debug.h"
#include "DexDefs.h"
#include "DexEncoding.h"
#include "RedexException.h"

class DexType;
class DexTypeList;
class DexString;
class DexFieldRef;
class DexMethodRef;
class DexProto;

class DexIdx {
 private:
  const uint8_t* m_dexbase;

  dex_string_id* m_string_ids;
  uint32_t m_string_ids_size;
  dex_type_id* m_type_ids;
  uint32_t m_type_ids_size;
  dex_field_id* m_field_ids;
  uint32_t m_field_ids_size;
  dex_method_id* m_method_ids;
  uint32_t m_method_ids_size;
  dex_proto_id* m_proto_ids;
  uint32_t m_proto_ids_size;

  DexString** m_string_cache;
  DexType** m_type_cache;
  DexFieldRef** m_field_cache;
  DexMethodRef** m_method_cache;
  DexProto** m_proto_cache;

  DexType* get_typeidx_fromdex(uint32_t typeidx);
  DexString* get_stringidx_fromdex(uint32_t stridx);
  DexFieldRef* get_fieldidx_fromdex(uint32_t fidx);
  DexMethodRef* get_methodidx_fromdex(uint32_t midx);
  DexProto* get_protoidx_fromdex(uint32_t pidx);

 public:
  explicit DexIdx(const dex_header* dh);
  ~DexIdx();

  DexString* get_stringidx(uint32_t stridx) {
    if (m_string_cache[stridx] == nullptr) {
      m_string_cache[stridx] = get_stringidx_fromdex(stridx);
    }
    assert(m_string_cache[stridx]);
    return m_string_cache[stridx];
  }

  DexString* get_nullable_stringidx(uint32_t stridx) {
    if (stridx == DEX_NO_INDEX) return nullptr;
    return get_stringidx(stridx);
  }

  DexType* get_typeidx(uint32_t typeidx) {
    if (typeidx == DEX_NO_INDEX) {
      return nullptr;
    }

    always_assert_type_log(
        typeidx < m_type_ids_size, RedexError::CACHE_INDEX_OUT_OF_BOUND,
        "Type index is out of bound. index: %d, cache size: %d", typeidx,
        m_type_ids_size);

    if (m_type_cache[typeidx] == nullptr) {
      m_type_cache[typeidx] = get_typeidx_fromdex(typeidx);
    }
    assert(m_type_cache[typeidx]);
    return m_type_cache[typeidx];
  }

  DexFieldRef* get_fieldidx(uint32_t fidx) {
    always_assert_type_log(
        fidx < m_field_ids_size, RedexError::CACHE_INDEX_OUT_OF_BOUND,
        "Field index is out of bound. index: %d, cache size: %d", fidx,
        m_field_ids_size);

    if (m_field_cache[fidx] == nullptr) {
      m_field_cache[fidx] = get_fieldidx_fromdex(fidx);
    }
    assert(m_field_cache[fidx]);
    return m_field_cache[fidx];
  }

  DexMethodRef* get_methodidx(uint32_t midx) {
    always_assert_type_log(
        midx < m_method_ids_size, RedexError::CACHE_INDEX_OUT_OF_BOUND,
        "Method index is out of bound. index: %d, cache size: %d", midx,
        m_method_ids_size);

    if (m_method_cache[midx] == nullptr) {
      m_method_cache[midx] = get_methodidx_fromdex(midx);
    }
    assert(m_method_cache[midx]);
    return m_method_cache[midx];
  }

  DexProto* get_protoidx(uint32_t pidx) {
    always_assert_type_log(
        pidx < m_proto_ids_size, RedexError::CACHE_INDEX_OUT_OF_BOUND,
        "Prototype index is out of bound. index: %d, cache size: %d", pidx,
        m_proto_ids_size);

    if (m_proto_cache[pidx] == nullptr) {
      m_proto_cache[pidx] = get_protoidx_fromdex(pidx);
    }
    assert(m_proto_cache[pidx]);
    return m_proto_cache[pidx];
  }

  const uint32_t* get_uint_data(uint32_t offset) {
    always_assert(offset < ((dex_header*)m_dexbase)->file_size);
    return (uint32_t*)(m_dexbase + offset);
  }

  const uint8_t* get_uleb_data(uint32_t offset) {
    always_assert(offset < ((dex_header*)m_dexbase)->file_size);
    return m_dexbase + offset;
  }

  DexTypeList* get_type_list(uint32_t offset);

  friend std::string show(DexIdx*);
};

inline DexString* decode_noindexable_string(DexIdx* idx,
                                            const uint8_t*& encdata) {
  DexString* str = nullptr;
  uint32_t sidx = read_uleb128p1(&encdata);
  if (sidx != DEX_NO_INDEX) {
    str = idx->get_stringidx(sidx);
  }
  return str;
}

inline DexType* decode_noindexable_type(DexIdx* idx, const uint8_t*& encdata) {
  DexType* type = nullptr;
  uint32_t tidx = read_uleb128p1(&encdata);
  if (tidx != DEX_NO_INDEX) {
    type = idx->get_typeidx(tidx);
  }
  return type;
}
