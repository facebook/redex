/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAnnotation.h"

#include <sstream>

#include "Debug.h"
#include "DexClass.h"
#include "DexIdx.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Show.h"

void DexEncodedValueMethodType::gather_strings(
    std::vector<const DexString*>& lstring) const {
  proto()->gather_strings(lstring);
}

void DexEncodedValueString::gather_strings(
    std::vector<const DexString*>& lstring) const {
  lstring.push_back(string());
}

void DexEncodedValueType::gather_types(std::vector<DexType*>& ltype) const {
  ltype.push_back(type());
}

void DexEncodedValueField::gather_fields(
    std::vector<DexFieldRef*>& lfield) const {
  lfield.push_back(field());
}

void DexEncodedValueMethod::gather_methods(
    std::vector<DexMethodRef*>& lmethod) const {
  lmethod.push_back(method());
}

void DexEncodedValueMethodHandle::gather_methods(
    std::vector<DexMethodRef*>& lmethod) const {
  methodhandle()->gather_methods(lmethod);
}

void DexEncodedValueMethodHandle::gather_fields(
    std::vector<DexFieldRef*>& lfield) const {
  methodhandle()->gather_fields(lfield);
}

void DexEncodedValueMethodHandle::gather_methodhandles(
    std::vector<DexMethodHandle*>& lhandles) const {
  lhandles.push_back(methodhandle());
}

void DexEncodedValueArray::gather_strings(
    std::vector<const DexString*>& lstring) const {
  for (auto& ev : *evalues()) {
    ev->gather_strings(lstring);
  }
}

void DexEncodedValueArray::gather_types(std::vector<DexType*>& ltype) const {
  for (auto& ev : *evalues()) {
    ev->gather_types(ltype);
  }
}

void DexEncodedValueArray::gather_fields(
    std::vector<DexFieldRef*>& lfield) const {
  for (auto& ev : *evalues()) {
    ev->gather_fields(lfield);
  }
}

void DexEncodedValueArray::gather_methods(
    std::vector<DexMethodRef*>& lmethod) const {
  for (auto& ev : *evalues()) {
    ev->gather_methods(lmethod);
  }
}

void DexEncodedValueAnnotation::gather_strings(
    std::vector<const DexString*>& lstring) const {
  for (auto const& elem : m_annotations) {
    lstring.push_back(elem.string);
    elem.encoded_value->gather_strings(lstring);
  }
}

void DexEncodedValueAnnotation::gather_types(
    std::vector<DexType*>& ltype) const {
  ltype.push_back(m_type);
  for (auto const& anno : m_annotations) {
    anno.encoded_value->gather_types(ltype);
  }
}

void DexEncodedValueAnnotation::gather_fields(
    std::vector<DexFieldRef*>& lfield) const {
  for (auto const& anno : m_annotations) {
    anno.encoded_value->gather_fields(lfield);
  }
}

void DexEncodedValueAnnotation::gather_methods(
    std::vector<DexMethodRef*>& lmethod) const {
  for (auto const& anno : m_annotations) {
    anno.encoded_value->gather_methods(lmethod);
  }
}

void DexAnnotation::gather_strings(
    std::vector<const DexString*>& lstring) const {
  for (auto const& anno : m_anno_elems) {
    lstring.push_back(anno.string);
    anno.encoded_value->gather_strings(lstring);
  }
}

void DexAnnotation::gather_types(std::vector<DexType*>& ltype) const {
  ltype.push_back(m_type);
  for (auto const& anno : m_anno_elems) {
    anno.encoded_value->gather_types(ltype);
  }
}

void DexAnnotation::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  for (auto const& anno : m_anno_elems) {
    anno.encoded_value->gather_fields(lfield);
  }
}

void DexAnnotation::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  for (auto const& anno : m_anno_elems) {
    anno.encoded_value->gather_methods(lmethod);
  }
}

void DexAnnotationSet::gather_strings(
    std::vector<const DexString*>& lstring) const {
  for (auto const& anno : m_annotations) {
    anno->gather_strings(lstring);
  }
}

void DexAnnotationSet::gather_types(std::vector<DexType*>& ltype) const {
  for (auto const& anno : m_annotations) {
    anno->gather_types(ltype);
  }
}

void DexAnnotationSet::gather_methods(
    std::vector<DexMethodRef*>& lmethod) const {
  for (auto const& anno : m_annotations) {
    anno->gather_methods(lmethod);
  }
}

void DexAnnotationSet::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  for (auto const& anno : m_annotations) {
    anno->gather_fields(lfield);
  }
}

uint64_t read_evarg(const uint8_t*& encdata,
                    uint8_t evarg,
                    bool sign_extend /* = false */
) {
  uint64_t v = *encdata++;
  int shift = 8;
  while (evarg--) {
    v |= uint64_t(*encdata++) << shift;
    shift += 8;
  }
  if (sign_extend) {
    v = (int64_t)(v << (64 - shift)) >> (64 - shift);
  }
  return v;
}

void type_encoder(uint8_t*& encdata, uint8_t type, uint64_t val) {
  uint8_t devtb = DEVT_HDR_TYPE(type);
  int count = 0;
  uint64_t t = (val >> 8);
  while (t) {
    count++;
    t >>= 8;
  }
  devtb |= TO_DEVT_HDR_ARG(count);
  *encdata++ = devtb;
  t = (val >> 8);
  *encdata++ = val & 0xff;
  while (t) {
    *encdata++ = t & 0xff;
    t >>= 8;
  }
}

void type_encoder_signext(uint8_t*& encdata, uint8_t type, uint64_t val) {
  uint8_t* mp = encdata++;
  int64_t sval = *(int64_t*)&val;
  int64_t t = sval;
  int bytes = 0;
  while (true) {
    uint8_t emit = t & 0xff;
    int64_t rest = t >> 8;
    *encdata++ = emit;
    bytes++;
    if (rest == 0) {
      if ((emit & 0x80) == 0) break;
    }
    if (rest == -1) {
      if ((emit & 0x80) == 0x80) break;
    }
    t = rest;
  }
  *mp = DEVT_HDR_TYPE(type) | TO_DEVT_HDR_ARG(bytes - 1);
}

void type_encoder_fp(uint8_t*& encdata, uint8_t type, uint64_t val) {
  // Ignore trailing zero bytes.
  int bytes = 0;
  while (val && ((val & 0xff) == 0)) {
    val >>= 8;
    bytes++;
  }
  int encbytes;
  switch (type) {
  case DEVT_FLOAT:
    encbytes = 4 - bytes;
    break;
  case DEVT_DOUBLE:
    encbytes = 8 - bytes;
    break;
  default:
    not_reached();
  }
  if (val == 0) {
    encbytes = 1;
  }
  // Encode.
  *encdata++ = DEVT_HDR_TYPE(type) | TO_DEVT_HDR_ARG(encbytes - 1);
  for (int i = 0; i < encbytes; i++) {
    *encdata++ = val & 0xff;
    val >>= 8;
  }
}

void DexEncodedValue::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  switch (m_evtype) {
  case DEVT_SHORT:
  case DEVT_INT:
  case DEVT_LONG:
    type_encoder_signext(encdata, m_evtype, m_val.m_value);
    return;
  case DEVT_FLOAT:
  case DEVT_DOUBLE:
    type_encoder_fp(encdata, m_evtype, m_val.m_value);
    return;
  default:
    // TOOD: Should this really be here?
    type_encoder(encdata, m_evtype, as_value());
    return;
  }
}
void DexEncodedValue::vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes) {
  // Relatively large buffer as Kotlin metadata annotations may be huge.
  constexpr size_t kRedZone = 1024;
  constexpr size_t kBufferSize = 16384;
  uint8_t buffer[kBufferSize];
  uint8_t* pend = buffer;
  encode(dodx, pend);
  always_assert_log((size_t)(pend - buffer) <= kBufferSize - kRedZone,
                    "DexEncodedValue::vencode overflow, size %d: %s",
                    (int)(pend - buffer), show().c_str());
  for (uint8_t* p = buffer; p < pend; p++) {
    bytes.push_back(*p);
  }
}

void DexEncodedValueBit::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint8_t devtb = DEVT_HDR_TYPE(m_evtype);
  if (m_val.m_value) {
    devtb |= TO_DEVT_HDR_ARG(1);
  }
  *encdata++ = devtb;
}

void DexEncodedValueString::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t sidx = dodx->stringidx(string());
  type_encoder(encdata, m_evtype, sidx);
}

void DexEncodedValueType::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t tidx = dodx->typeidx(type());
  type_encoder(encdata, m_evtype, tidx);
}

void DexEncodedValueField::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t fidx = dodx->fieldidx(field());
  type_encoder(encdata, m_evtype, fidx);
}

void DexEncodedValueMethod::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t midx = dodx->methodidx(method());
  type_encoder(encdata, m_evtype, midx);
}

void DexEncodedValueMethodType::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t pidx = dodx->protoidx(proto());
  type_encoder(encdata, m_evtype, pidx);
}

void DexEncodedValueMethodHandle::encode(DexOutputIdx* dodx,
                                         uint8_t*& encdata) {
  uint32_t mhidx = dodx->methodhandleidx(methodhandle());
  type_encoder(encdata, m_evtype, mhidx);
}

void DexEncodedValueArray::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  /*
   * Static values are implied to be DEVT_ARRAY, and thus don't
   * have a type byte.
   */
  if (!m_static_val) {
    uint8_t devtb = DEVT_HDR_TYPE(m_evtype);
    *encdata++ = devtb;
  }
  encdata = write_uleb128(encdata, (uint32_t)evalues()->size());
  for (auto const& ev : *evalues()) {
    ev->encode(dodx, encdata);
  }
}

void DexEncodedValueAnnotation::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint8_t devtb = DEVT_HDR_TYPE(m_evtype);
  uint32_t tidx = dodx->typeidx(m_type);
  *encdata++ = devtb;
  encdata = write_uleb128(encdata, tidx);
  encdata = write_uleb128(encdata, (uint32_t)m_annotations.size());
  for (auto const& dae : m_annotations) {
    auto str = dae.string;
    DexEncodedValue* dev = dae.encoded_value.get();
    uint32_t sidx = dodx->stringidx(str);
    encdata = write_uleb128(encdata, sidx);
    dev->encode(dodx, encdata);
  }
}

static DexAnnotationElement get_annotation_element(DexIdx* idx,
                                                   const uint8_t*& encdata) {
  uint32_t sidx = read_uleb128(&encdata);
  auto name = idx->get_stringidx(sidx);
  always_assert_log(name != nullptr,
                    "Invalid string idx in annotation element");
  return DexAnnotationElement(name,
                              DexEncodedValue::get_encoded_value(idx, encdata));
}

std::unique_ptr<DexEncodedValueArray> get_encoded_value_array(
    DexIdx* idx, const uint8_t*& encdata) {
  uint32_t size = read_uleb128(&encdata);
  auto* evlist = new std::vector<std::unique_ptr<DexEncodedValue>>();
  evlist->reserve(size);
  for (uint32_t i = 0; i < size; i++) {
    evlist->emplace_back(DexEncodedValue::get_encoded_value(idx, encdata));
  }
  return std::make_unique<DexEncodedValueArray>(evlist);
}

bool DexEncodedValue::is_evtype_primitive() const {
  switch (m_evtype) {
  case DEVT_BYTE:
  case DEVT_SHORT:
  case DEVT_CHAR:
  case DEVT_INT:
  case DEVT_LONG:
  case DEVT_FLOAT:
  case DEVT_DOUBLE:
  // case DEVT_NULL:
  case DEVT_BOOLEAN:
    return true;
  default:
    return false;
  };
}

bool DexEncodedValue::is_zero() const {
  switch (m_evtype) {
  case DEVT_BYTE:
  case DEVT_SHORT:
  case DEVT_CHAR:
  case DEVT_INT:
  case DEVT_LONG:
  case DEVT_FLOAT:
  case DEVT_DOUBLE:
  case DEVT_BOOLEAN:
    return m_val.m_value == 0;
  case DEVT_NULL:
    return true;
  default:
    return false;
  }
}

bool DexEncodedValue::is_wide() const {
  return m_evtype == DEVT_LONG || m_evtype == DEVT_DOUBLE;
}

std::unique_ptr<DexEncodedValue> DexEncodedValue::zero_for_type(DexType* type) {
  if (type == type::_byte()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_BYTE, (uint64_t)0));
  } else if (type == type::_char()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_CHAR, (uint64_t)0));
  } else if (type == type::_short()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_SHORT, (uint64_t)0));
  } else if (type == type::_int()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_INT, (uint64_t)0));
  } else if (type == type::_long()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_LONG, (uint64_t)0));
  } else if (type == type::_float()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_FLOAT, (uint64_t)0));
  } else if (type == type::_double()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(DEVT_DOUBLE, (uint64_t)0));
  } else if (type == type::_boolean()) {
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueBit(DEVT_BOOLEAN, false));
  } else {
    // not a primitive
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueBit(DEVT_NULL, false));
  }
}

std::unique_ptr<DexEncodedValue> DexEncodedValue::get_encoded_value(
    DexIdx* idx, const uint8_t*& encdata) {
  uint8_t evhdr = *encdata++;
  DexEncodedValueTypes evt = (DexEncodedValueTypes)DEVT_HDR_TYPE(evhdr);
  uint8_t evarg = DEVT_HDR_ARG(evhdr);
  switch (evt) {
  case DEVT_SHORT:
  case DEVT_INT:
  case DEVT_LONG: {
    uint64_t v = read_evarg(encdata, evarg, true /* sign_extend */);
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(evt, v));
  }
  case DEVT_BYTE:
  case DEVT_CHAR: {
    uint64_t v = read_evarg(encdata, evarg, false /* sign_extend */);
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(evt, v));
  }
  case DEVT_FLOAT: {
    // We sign extend floats so that they can be treated just like signed ints
    uint64_t v = read_evarg(encdata, evarg, true /* sign_extend */)
                 << ((3 - evarg) * 8);
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(evt, v));
  }
  case DEVT_DOUBLE: {
    uint64_t v = read_evarg(encdata, evarg, false /* sign_extend */)
                 << ((7 - evarg) * 8);
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValuePrimitive(evt, v));
  }
  case DEVT_METHOD_TYPE: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexProto* evproto = idx->get_protoidx(evidx);
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueMethodType(evproto));
  }
  case DEVT_METHOD_HANDLE: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexMethodHandle* evmethodhandle = idx->get_methodhandleidx(evidx);
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueMethodHandle(evmethodhandle));
  }

  case DEVT_NULL:
    return std::unique_ptr<DexEncodedValue>(new DexEncodedValueBit(evt, false));
  case DEVT_BOOLEAN:
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueBit(evt, evarg > 0));
  case DEVT_STRING: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    auto evstring = idx->get_stringidx(evidx);
    always_assert_log(evstring != nullptr,
                      "Invalid string idx in annotation element");
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueString(evstring));
  }
  case DEVT_TYPE: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexType* evtype = idx->get_typeidx(evidx);
    always_assert_log(evtype != nullptr,
                      "Invalid type idx in annotation element");
    return std::unique_ptr<DexEncodedValue>(new DexEncodedValueType(evtype));
  }
  case DEVT_FIELD:
  case DEVT_ENUM: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexFieldRef* evfield = idx->get_fieldidx(evidx);
    always_assert_log(evfield != nullptr,
                      "Invalid field idx in annotation element");
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueField(evt, evfield));
  }
  case DEVT_METHOD: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexMethodRef* evmethod = idx->get_methodidx(evidx);
    always_assert_log(evmethod != nullptr,
                      "Invalid method idx in annotation element");
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueMethod(evmethod));
  }
  case DEVT_ARRAY:
    return get_encoded_value_array(idx, encdata);
  case DEVT_ANNOTATION: {
    EncodedAnnotations eanno{};
    uint32_t tidx = read_uleb128(&encdata);
    uint32_t count = read_uleb128(&encdata);
    DexType* type = idx->get_typeidx(tidx);
    always_assert_log(type != nullptr,
                      "Invalid DEVT_ANNOTATION within annotation type");
    eanno.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
      eanno.emplace_back(get_annotation_element(idx, encdata));
    }
    return std::unique_ptr<DexEncodedValue>(
        new DexEncodedValueAnnotation(type, std::move(eanno)));
  }
  };
  not_reached_log("Bogus annotation");
}

std::unique_ptr<DexAnnotation> DexAnnotation::get_annotation(
    DexIdx* idx, uint32_t anno_off) {
  if (anno_off == 0) return nullptr;
  const uint8_t* encdata = idx->get_uleb_data(anno_off);
  uint8_t viz = *encdata++;
  always_assert_log(viz <= DAV_SYSTEM, "Invalid annotation visibility %d", viz);
  uint32_t tidx = read_uleb128(&encdata);
  uint32_t count = read_uleb128(&encdata);
  DexType* type = idx->get_typeidx(tidx);
  always_assert_log(type != nullptr, "Invalid annotation type");
  auto anno =
      std::make_unique<DexAnnotation>(type, (DexAnnotationVisibility)viz);
  anno->m_anno_elems.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    anno->m_anno_elems.emplace_back(get_annotation_element(idx, encdata));
  }
  return anno;
}

void DexAnnotation::add_element(const char* key,
                                std::unique_ptr<DexEncodedValue> value) {
  m_anno_elems.emplace_back(DexString::make_string(key), std::move(value));
}
void DexAnnotation::add_element(DexAnnotationElement elem) {
  m_anno_elems.emplace_back(std::move(elem));
}

std::unique_ptr<DexAnnotationSet> DexAnnotationSet::get_annotation_set(
    DexIdx* idx, uint32_t aset_off) {
  if (aset_off == 0) return nullptr;
  const uint32_t* adata = idx->get_uint_data(aset_off);
  auto aset = std::make_unique<DexAnnotationSet>();
  uint32_t count = *adata++;

  aset->m_annotations.reserve(count - std::count(adata, adata + count, 0));

  for (uint32_t i = 0; i < count; i++) {
    uint32_t off = adata[i];
    auto anno = DexAnnotation::get_annotation(idx, off);
    if (anno != nullptr) {
      aset->m_annotations.emplace_back(std::move(anno));
    }
  }
  return aset;
}

void DexAnnotationDirectory::calc_internals() {
  int cntviz = 0;
  auto updateCount = [this](DexAnnotationSet* das) {
    unsigned long ca, cv;
    das->viz_counts(ca, cv);
    m_anno_count += ca;
    m_aset_size += 4 + 4 * (ca);
    m_aset_count++;
    return cv;
  };
  if (m_class) {
    cntviz += updateCount(m_class);
  }
  if (m_field) {
    for (auto const& p : *m_field) {
      DexAnnotationSet* das = p.second;
      cntviz += updateCount(das);
    }
  }
  if (m_method) {
    for (auto const& p : *m_method) {
      DexAnnotationSet* das = p.second;
      cntviz += updateCount(das);
    }
  }
  if (m_method_param) {
    for (auto const& p : *m_method_param) {
      ParamAnnotations* pa = p.second;
      m_xref_size += 4 + 4 * pa->size();
      m_xref_count++;
      for (auto const& pp : *pa) {
        auto& das = pp.second;
        cntviz += updateCount(das.get());
      }
    }
  }
  if (m_anno_count != 0) {
    m_viz = (double)cntviz / m_anno_count;
  }
}

bool method_annotation_compare(std::pair<DexMethod*, DexAnnotationSet*> a,
                               std::pair<DexMethod*, DexAnnotationSet*> b) {
  return compare_dexmethods(a.first, b.first);
}

bool method_param_annotation_compare(
    std::pair<DexMethod*, ParamAnnotations*> a,
    std::pair<DexMethod*, ParamAnnotations*> b) {
  return compare_dexmethods(a.first, b.first);
}

bool field_annotation_compare(std::pair<DexFieldRef*, DexAnnotationSet*> a,
                              std::pair<DexFieldRef*, DexAnnotationSet*> b) {
  return compare_dexfields(a.first, b.first);
}

void DexAnnotationDirectory::gather_asets(
    std::vector<DexAnnotationSet*>& aset) {
  if (m_class) aset.push_back(m_class);
  if (m_field) {
    for (auto fanno : *m_field)
      aset.push_back(fanno.second);
  }
  if (m_method) {
    for (auto manno : *m_method)
      aset.push_back(manno.second);
  }
  if (m_method_param) {
    for (auto mpanno : *m_method_param) {
      ParamAnnotations* params = mpanno.second;
      for (auto& param : *params)
        aset.push_back(param.second.get());
    }
  }
}

void DexAnnotationDirectory::gather_xrefs(
    std::vector<ParamAnnotations*>& xrefs) {
  if (m_method_param) {
    std::sort(m_method_param->begin(),
              m_method_param->end(),
              method_param_annotation_compare);
    for (auto param : *m_method_param) {
      ParamAnnotations* pa = param.second;
      xrefs.push_back(pa);
    }
  }
}

void DexAnnotationDirectory::gather_annotations(
    std::vector<DexAnnotation*>& alist) {
  if (m_class) m_class->gather_annotations(alist);
  if (m_field) {
    for (auto fanno : *m_field) {
      DexAnnotationSet* das = fanno.second;
      das->gather_annotations(alist);
    }
  }
  if (m_method) {
    for (auto manno : *m_method) {
      DexAnnotationSet* das = manno.second;
      das->gather_annotations(alist);
    }
  }
  if (m_method_param) {
    for (auto mpanno : *m_method_param) {
      ParamAnnotations* params = mpanno.second;
      for (auto& param : *params) {
        auto& das = param.second;
        das->gather_annotations(alist);
      }
    }
  }
}

void DexAnnotationDirectory::vencode(
    DexOutputIdx* dodx,
    std::vector<uint32_t>& annodirout,
    std::map<ParamAnnotations*, uint32_t>& xrefmap,
    std::map<DexAnnotationSet*, uint32_t>& asetmap) {
  uint32_t classoff = 0;
  uint32_t cntaf = 0;
  uint32_t cntam = 0;
  uint32_t cntamp = 0;
  if (m_class) {
    always_assert_log(asetmap.count(m_class) != 0, "Uninitialized aset %p '%s'",
                      m_class, show(m_class).c_str());
    classoff = asetmap[m_class];
  }
  if (m_field) {
    cntaf = (uint32_t)m_field->size();
  }
  if (m_method) {
    cntam = (uint32_t)m_method->size();
  }
  if (m_method_param) {
    cntamp = (uint32_t)m_method_param->size();
  }
  annodirout.push_back(classoff);
  annodirout.push_back(cntaf);
  annodirout.push_back(cntam);
  annodirout.push_back(cntamp);
  if (m_field) {
    /* Optimization note:
     * A tape sort could be used instead as there are two different
     * ordered lists here.
     */
    std::sort(m_field->begin(), m_field->end(), field_annotation_compare);
    for (auto const& p : *m_field) {
      DexAnnotationSet* das = p.second;
      annodirout.push_back(dodx->fieldidx(p.first));
      always_assert_log(asetmap.count(das) != 0, "Uninitialized aset %p '%s'",
                        das, show(das).c_str());
      annodirout.push_back(asetmap[das]);
    }
  }
  if (m_method) {
    std::sort(m_method->begin(), m_method->end(), method_annotation_compare);
    for (auto const& p : *m_method) {
      DexMethod* m = p.first;
      DexAnnotationSet* das = p.second;
      uint32_t midx = dodx->methodidx(m);
      annodirout.push_back(midx);
      always_assert_log(asetmap.count(das) != 0, "Uninitialized aset %p '%s'",
                        das, show(das).c_str());
      annodirout.push_back(asetmap[das]);
    }
  }
  if (m_method_param) {
    std::sort(m_method_param->begin(),
              m_method_param->end(),
              method_param_annotation_compare);
    for (auto const& p : *m_method_param) {
      ParamAnnotations* pa = p.second;
      annodirout.push_back(dodx->methodidx(p.first));
      always_assert_log(xrefmap.count(pa) != 0,
                        "Uninitialized ParamAnnotations %p", pa);
      annodirout.push_back(xrefmap[pa]);
    }
  }
}

void DexAnnotationSet::gather_annotations(std::vector<DexAnnotation*>& list) {
  for (auto& annotation : m_annotations) {
    list.push_back(annotation.get());
  }
}

void DexAnnotationSet::vencode(DexOutputIdx* dodx,
                               std::vector<uint32_t>& asetout,
                               std::map<DexAnnotation*, uint32_t>& annoout) {
  asetout.push_back((uint32_t)m_annotations.size());
  std::sort(m_annotations.begin(), m_annotations.end(),
            [](const auto& a, const auto& b) {
              return compare_dextypes(a->type(), b->type());
            });
  for (auto& anno : m_annotations) {
    always_assert_log(annoout.count(anno.get()) != 0,
                      "Uninitialized annotation %p '%s', bailing\n",
                      anno.get(),
                      show(anno.get()).c_str());
    asetout.push_back(annoout[anno.get()]);
  }
}

static void uleb_append(std::vector<uint8_t>& bytes, uint32_t v) {
  uint8_t tarray[5];
  uint8_t* pend = write_uleb128(tarray, v);
  for (uint8_t* p = tarray; p < pend; p++) {
    bytes.push_back(*p);
  }
}

void DexAnnotation::vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes) {
  bytes.push_back(m_viz);
  uleb_append(bytes, dodx->typeidx(m_type));
  uleb_append(bytes, (uint32_t)m_anno_elems.size());
  for (auto& elem : m_anno_elems) {
    auto string = elem.string;
    DexEncodedValue* ev = elem.encoded_value.get();
    uleb_append(bytes, dodx->stringidx(string));
    ev->vencode(dodx, bytes);
  }
}

namespace {
std::string show_helper(const DexEncodedValueArray* a, bool deobfuscated) {
  std::ostringstream ss;
  ss << (a->is_static_val() ? "(static) " : "");
  if (a->evalues()) {
    bool first = true;
    for (const auto& evalue : *a->evalues()) {
      if (!first) {
        ss << ' ';
      }
      if (deobfuscated) {
        ss << evalue->show_deobfuscated();
      } else {
        ss << evalue->show();
      }
      first = false;
    }
  }
  return ss.str();
}

std::string show_helper(const EncodedAnnotations* annos, bool deobfuscated) {
  if (!annos) {
    return "";
  }
  std::ostringstream ss;
  bool first = true;
  for (auto const& pair : *annos) {
    if (!first) {
      ss << ", ";
    }
    ss << show(pair.string) << ":";
    if (deobfuscated) {
      ss << pair.encoded_value->show_deobfuscated();
    } else {
      ss << pair.encoded_value->show();
    }
    first = false;
  }
  return ss.str();
}
} // namespace

std::string show(const EncodedAnnotations* annos) {
  return show_helper(annos, false);
}

std::string show_deobfuscated(const EncodedAnnotations* annos) {
  return show_helper(annos, true);
}

std::string show(const EncodedAnnotations& annos) {
  return show_helper(&annos, false);
}

std::string show_deobfuscated(const EncodedAnnotations& annos) {
  return show_helper(&annos, true);
}

std::string DexEncodedValue::show() const {
  std::ostringstream ss;
  ss << as_value();
  return ss.str();
}

std::string DexEncodedValueAnnotation::show() const {
  std::ostringstream ss;
  ss << "type:" << ::show(m_type) << " annotations:" << ::show(m_annotations);
  return ss.str();
}

std::string DexEncodedValueAnnotation::show_deobfuscated() const {
  std::ostringstream ss;
  ss << "type:" << ::show(m_type)
     << " annotations:" << ::show_deobfuscated(m_annotations);
  return ss.str();
}

std::string DexEncodedValueArray::show() const {
  return show_helper(this, false);
}

std::string DexEncodedValueArray::show_deobfuscated() const {
  return show_helper(this, true);
}

std::string DexEncodedValueString::show() const { return ::show(string()); }

std::string DexEncodedValueType::show() const { return ::show(type()); }

std::string DexEncodedValueField::show() const { return ::show(field()); }
std::string DexEncodedValueField::show_deobfuscated() const {
  return ::show_deobfuscated(field());
}

std::string DexEncodedValueMethod::show() const { return ::show(method()); }
std::string DexEncodedValueMethod::show_deobfuscated() const {
  return ::show_deobfuscated(method());
}

std::string DexEncodedValueMethodType::show() const { return ::show(proto()); }
std::string DexEncodedValueMethodType::show_deobfuscated() const {
  return ::show_deobfuscated(proto());
}

std::string DexEncodedValueMethodHandle::show() const {
  return ::show(methodhandle());
}
std::string DexEncodedValueMethodHandle::show_deobfuscated() const {
  // TODO(T58570881) - fix deobfuscation
  return ::show(methodhandle());
}
