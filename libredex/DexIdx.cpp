/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexIdx.h"

#include <sstream>

#include "DexCallSite.h"
#include "DexClass.h"
#include "DexMethodHandle.h"

#define INIT_DMAP_ID(TYPE)                                             \
  always_assert_log(dh->TYPE##_ids_off < dh->file_size,                \
                    #TYPE " section offset out of range");             \
  m_##TYPE##_ids = (dex_##TYPE##_id*)(m_dexbase + dh->TYPE##_ids_off); \
  m_##TYPE##_ids_size = dh->TYPE##_ids_size;                           \
  m_##TYPE##_cache.resize(dh->TYPE##_ids_size)

DexIdx::DexIdx(const dex_header* dh) {
  m_dexbase = (const uint8_t*)dh;
  INIT_DMAP_ID(string);
  INIT_DMAP_ID(type);
  INIT_DMAP_ID(field);
  INIT_DMAP_ID(method);
  INIT_DMAP_ID(proto);

  dex_map_list* map_list = (dex_map_list*)(m_dexbase + dh->map_off);
  for (uint32_t i = 0; i < map_list->size; i++) {
    auto& item = map_list->items[i];
    switch (item.type) {
    case TYPE_CALL_SITE_ID_ITEM: {
      dex_callsite_id* callsite_ids =
          (dex_callsite_id*)((uint8_t*)dh + item.offset);
      m_callsite_ids = callsite_ids;
      m_callsite_ids_size = item.size;
      m_callsite_cache.resize(m_callsite_ids_size);
    } break;
    case TYPE_METHOD_HANDLE_ITEM: {
      dex_methodhandle_id* methodhandle_ids =
          (dex_methodhandle_id*)((uint8_t*)dh + item.offset);
      m_methodhandle_ids = methodhandle_ids;
      m_methodhandle_ids_size = item.size;
      m_methodhandle_cache.resize(m_methodhandle_ids_size);
    } break;
    }
  }
}

DexCallSite* DexIdx::get_callsiteidx_fromdex(uint32_t csidx) {
  redex_assert(csidx < m_callsite_ids_size);
  // callsites are indirected through the callsite_id table, because
  // they are variable length. so first find the real offset of this
  // callsite
  const uint8_t* callsite_data = m_dexbase + m_callsite_ids[csidx].callsite_off;
  auto callsite_eva = get_encoded_value_array(this, callsite_data);
  auto evalues = callsite_eva->evalues();
  DexEncodedValue* ev_linker_method_handle = evalues->at(0);
  always_assert_log(ev_linker_method_handle->evtype() == DEVT_METHOD_HANDLE,
                    "Unexpected evtype callsite item arg 0: %d",
                    ev_linker_method_handle->evtype());
  DexEncodedValue* ev_linker_method_name = evalues->at(1);
  always_assert_log(ev_linker_method_name->evtype() == DEVT_STRING,
                    "Unexpected evtype callsite item arg 1: %d",
                    ev_linker_method_name->evtype());
  DexEncodedValue* ev_linker_method_type = evalues->at(2);
  always_assert_log(ev_linker_method_type->evtype() == DEVT_METHOD_TYPE,
                    "Unexpected evtype callsite item arg 2: %d",
                    ev_linker_method_type->evtype());
  DexMethodHandle* linker_method_handle =
      ((DexEncodedValueMethodHandle*)ev_linker_method_handle)->methodhandle();
  auto linker_method_name =
      ((DexEncodedValueString*)ev_linker_method_name)->string();
  DexProto* linker_method_proto =
      ((DexEncodedValueMethodType*)ev_linker_method_type)->proto();
  std::vector<DexEncodedValue*> linker_args;
  for (unsigned long i = 3; i < evalues->size(); ++i) {
    DexEncodedValue* ev = evalues->at(i);
    linker_args.emplace_back(ev);
  }
  auto callsite = new DexCallSite(linker_method_handle,
                                  linker_method_name,
                                  linker_method_proto,
                                  linker_args);
  return callsite;
}

DexMethodHandle* DexIdx::get_methodhandleidx_fromdex(uint32_t mhidx) {
  redex_assert(mhidx < m_methodhandle_ids_size);
  MethodHandleType method_handle_type =
      (MethodHandleType)m_methodhandle_ids[mhidx].method_handle_type;
  if (DexMethodHandle::isInvokeType(method_handle_type)) {
    DexMethodRef* methodref =
        get_methodidx(m_methodhandle_ids[mhidx].field_or_method_id);
    return new DexMethodHandle(method_handle_type, methodref);
  } else {
    DexFieldRef* fieldref =
        get_fieldidx(m_methodhandle_ids[mhidx].field_or_method_id);
    return new DexMethodHandle(method_handle_type, fieldref);
  }
}

const DexString* DexIdx::get_stringidx_fromdex(uint32_t stridx) {
  redex_assert(stridx < m_string_ids_size);
  uint32_t stroff = m_string_ids[stridx].offset;
  always_assert_log(stroff < ((dex_header*)m_dexbase)->file_size,
                    "String data offset out of range");
  const uint8_t* dstr = m_dexbase + stroff;
  /* Strip off uleb128 size encoding */
  uint32_t utfsize = read_uleb128(&dstr);
  auto ret = DexString::make_string((const char*)dstr);
  always_assert_log(
      ret->length() == utfsize,
      "Parsed string UTF size is not the same as stringidx size. %u != %u",
      ret->length(),
      utfsize);
  return ret;
}

DexType* DexIdx::get_typeidx_fromdex(uint32_t typeidx) {
  redex_assert(typeidx < m_type_ids_size);
  uint32_t stridx = m_type_ids[typeidx].string_idx;
  auto dexstr = get_stringidx(stridx);
  return DexType::make_type(dexstr);
}

DexFieldRef* DexIdx::get_fieldidx_fromdex(uint32_t fidx) {
  redex_assert(fidx < m_field_ids_size);
  DexType* container = get_typeidx(m_field_ids[fidx].classidx);
  DexType* ftype = get_typeidx(m_field_ids[fidx].typeidx);
  auto name = get_stringidx(m_field_ids[fidx].nameidx);
  return DexField::make_field(container, name, ftype);
}

DexMethodRef* DexIdx::get_methodidx_fromdex(uint32_t midx) {
  redex_assert(midx < m_method_ids_size);
  DexType* container = get_typeidx(m_method_ids[midx].classidx);
  DexProto* proto = get_protoidx(m_method_ids[midx].protoidx);
  auto name = get_stringidx(m_method_ids[midx].nameidx);
  return DexMethod::make_method(container, name, proto);
}

DexProto* DexIdx::get_protoidx_fromdex(uint32_t pidx) {
  redex_assert(pidx < m_proto_ids_size);
  DexType* rtype = get_typeidx(m_proto_ids[pidx].rtypeidx);
  auto shorty = get_stringidx(m_proto_ids[pidx].shortyidx);
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
  DexTypeList::ContainerType tlist;
  tlist.reserve(size);
  for (uint32_t i = 0; i < size; i++) {
    tlist.push_back(get_typeidx(typep[i]));
  }
  return DexTypeList::make_type_list(std::move(tlist));
}
