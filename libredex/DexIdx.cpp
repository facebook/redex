/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexIdx.h"

#include <sstream>

#include "DexClass.h"

#define INIT_DMAP_ID(TYPE, CACHETYPE)                                   \
  always_assert_log(                                                    \
    dh->TYPE##_ids_off < dh->file_size,                                 \
    #TYPE " section offset out of range");                              \
  m_##TYPE##_ids = (dex_##TYPE##_id*)(m_dexbase + dh->TYPE##_ids_off);  \
  m_##TYPE##_ids_size = dh->TYPE##_ids_size;                            \
  m_##TYPE##_cache = (CACHETYPE*)calloc(dh->TYPE##_ids_size, sizeof(CACHETYPE))

DexIdx::DexIdx(const dex_header* dh) {
  m_dexbase = (const uint8_t*)dh;
  INIT_DMAP_ID(string, DexString*);
  INIT_DMAP_ID(type, DexType*);
  INIT_DMAP_ID(field, DexFieldRef*);
  INIT_DMAP_ID(method, DexMethodRef*);
  INIT_DMAP_ID(proto, DexProto*);
}

DexIdx::~DexIdx() {
  free(m_string_cache);
  free(m_type_cache);
  free(m_field_cache);
  free(m_method_cache);
  free(m_proto_cache);
}

DexString* DexIdx::get_stringidx_fromdex(uint32_t stridx) {
  redex_assert(stridx < m_string_ids_size);
  uint32_t stroff = m_string_ids[stridx].offset;
  always_assert_log(
    stroff < ((dex_header*)m_dexbase)->file_size,
    "String data offset out of range");
  const uint8_t* dstr = m_dexbase + stroff;
  /* Strip off uleb128 size encoding */
  int utfsize = read_uleb128(&dstr);
  return DexString::make_string((const char*)dstr, utfsize);
}

DexType* DexIdx::get_typeidx_fromdex(uint32_t typeidx) {
  redex_assert(typeidx < m_type_ids_size);
  uint32_t stridx = m_type_ids[typeidx].string_idx;
  DexString* dexstr = get_stringidx(stridx);
  return DexType::make_type(dexstr);
}

DexFieldRef* DexIdx::get_fieldidx_fromdex(uint32_t fidx) {
  redex_assert(fidx < m_field_ids_size);
  DexType* container = get_typeidx(m_field_ids[fidx].classidx);
  DexType* ftype = get_typeidx(m_field_ids[fidx].typeidx);
  DexString* name = get_stringidx(m_field_ids[fidx].nameidx);
  return DexField::make_field(container, name, ftype);
}

DexMethodRef* DexIdx::get_methodidx_fromdex(uint32_t midx) {
  redex_assert(midx < m_method_ids_size);
  DexType* container = get_typeidx(m_method_ids[midx].classidx);
  DexProto* proto = get_protoidx(m_method_ids[midx].protoidx);
  DexString* name = get_stringidx(m_method_ids[midx].nameidx);
  return DexMethod::make_method(container, name, proto);
}

DexProto* DexIdx::get_protoidx_fromdex(uint32_t pidx) {
  redex_assert(pidx < m_proto_ids_size);
  DexType* rtype = get_typeidx(m_proto_ids[pidx].rtypeidx);
  DexString* shorty = get_stringidx(m_proto_ids[pidx].shortyidx);
  DexTypeList* args = get_type_list(m_proto_ids[pidx].param_off);
  return DexProto::make_proto(rtype, args, shorty);
}

DexTypeList* DexIdx::get_type_list(uint32_t offset) {
  if (offset == 0) {
    return DexTypeList::make_type_list({});
  }
  const uint32_t* tlp = get_uint_data(offset);
  uint32_t size = *tlp++;
  const uint16_t* typep = (const uint16_t*)tlp;
  std::deque<DexType*> tlist;
  for (uint32_t i = 0; i < size; i++) {
    tlist.push_back(get_typeidx(typep[i]));
  }
  return DexTypeList::make_type_list(std::move(tlist));
}
