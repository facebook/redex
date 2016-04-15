/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexClass.h"
#include "DexAnnotation.h"
#include "Debug.h"
#include "DexIdx.h"
#include "DexOutput.h"

void DexEncodedValueString::gather_strings(
    std::vector<DexString*>& lstring) {
  lstring.push_back(m_string);
}

void DexEncodedValueType::gather_types(std::vector<DexType*>& ltype) {
  ltype.push_back(m_type);
}

void DexEncodedValueField::gather_fields(std::vector<DexField*>& lfield) {
  lfield.push_back(m_field);
}

void DexEncodedValueMethod::gather_methods(
    std::vector<DexMethod*>& lmethod) {
  lmethod.push_back(m_method);
}

void DexEncodedValueArray::gather_strings(
    std::vector<DexString*>& lstring) {
  for (auto ev : *m_evalues) {
    ev->gather_strings(lstring);
  }
}

void DexEncodedValueArray::gather_types(std::vector<DexType*>& ltype) {
  for (auto ev : *m_evalues) {
    ev->gather_types(ltype);
  }
}

void DexEncodedValueArray::gather_fields(std::vector<DexField*>& lfield) {
  for (auto ev : *m_evalues) {
    ev->gather_fields(lfield);
  }
}

void DexEncodedValueArray::gather_methods(
    std::vector<DexMethod*>& lmethod) {
  for (auto ev : *m_evalues) {
    ev->gather_methods(lmethod);
  }
}

void DexEncodedValueAnnotation::gather_strings(
    std::vector<DexString*>& lstring) {
  for (auto const& elem : *m_annotations) {
    lstring.push_back(elem.string);
    elem.encoded_value->gather_strings(lstring);
  }
}

void DexEncodedValueAnnotation::gather_types(
    std::vector<DexType*>& ltype) {
  ltype.push_back(m_type);
  for (auto const& anno : *m_annotations) {
    anno.encoded_value->gather_types(ltype);
  }
}

void DexEncodedValueAnnotation::gather_fields(
    std::vector<DexField*>& lfield) {
  for (auto const& anno : *m_annotations) {
    anno.encoded_value->gather_fields(lfield);
  }
}

void DexEncodedValueAnnotation::gather_methods(
    std::vector<DexMethod*>& lmethod) {
  for (auto const& anno : *m_annotations) {
    anno.encoded_value->gather_methods(lmethod);
  }
}

void DexAnnotation::gather_strings(std::vector<DexString*>& lstring) {
  for (auto const& anno : m_anno_elems) {
    lstring.push_back(anno.string);
    anno.encoded_value->gather_strings(lstring);
  }
}

void DexAnnotation::gather_types(std::vector<DexType*>& ltype) {
  ltype.push_back(m_type);
  for (auto const& anno : m_anno_elems) {
    anno.encoded_value->gather_types(ltype);
  }
}

void DexAnnotation::gather_fields(std::vector<DexField*>& lfield) {
  for (auto const& anno : m_anno_elems) {
    anno.encoded_value->gather_fields(lfield);
  }
}

void DexAnnotation::gather_methods(std::vector<DexMethod*>& lmethod) {
  for (auto const& anno : m_anno_elems) {
    anno.encoded_value->gather_methods(lmethod);
  }
}

void DexAnnotationSet::gather_strings(std::vector<DexString*>& lstring) {
  for (auto const& anno : m_annotations) {
    anno->gather_strings(lstring);
  }
}

void DexAnnotationSet::gather_types(std::vector<DexType*>& ltype) {
  for (auto const& anno : m_annotations) {
    anno->gather_types(ltype);
  }
}

void DexAnnotationSet::gather_methods(std::vector<DexMethod*>& lmethod) {
  for (auto const& anno : m_annotations) {
    anno->gather_methods(lmethod);
  }
}

void DexAnnotationSet::gather_fields(std::vector<DexField*>& lfield) {
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
    assert(false);
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
    type_encoder_signext(encdata, m_evtype, m_value);
    return;
  case DEVT_FLOAT:
  case DEVT_DOUBLE:
    type_encoder_fp(encdata, m_evtype, m_value);
    return;
  default:
    type_encoder(encdata, m_evtype, m_value);
    return;
  }
}
#define MAX_BUFFER_SIZE (4096)
void DexEncodedValue::vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes) {
  uint8_t buffer[MAX_BUFFER_SIZE];
  uint8_t* pend = buffer;
  encode(dodx, pend);
  always_assert_log(pend - buffer <= MAX_BUFFER_SIZE,
                    "DexEncodedValue::vencode overflow, size %d\n",
                    (int)(pend - buffer));
  for (uint8_t* p = buffer; p < pend; p++) {
    bytes.push_back(*p);
  }
}

void DexEncodedValueBit::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint8_t devtb = DEVT_HDR_TYPE(m_evtype);
  if (m_value) {
    devtb |= TO_DEVT_HDR_ARG(1);
  }
  *encdata++ = devtb;
}

void DexEncodedValueString::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t sidx = dodx->stringidx(m_string);
  type_encoder(encdata, m_evtype, sidx);
}

void DexEncodedValueType::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t tidx = dodx->typeidx(m_type);
  type_encoder(encdata, m_evtype, tidx);
}

void DexEncodedValueField::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t fidx = dodx->fieldidx(m_field);
  type_encoder(encdata, m_evtype, fidx);
}

void DexEncodedValueMethod::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint32_t midx = dodx->methodidx(m_method);
  type_encoder(encdata, m_evtype, midx);
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
  encdata = write_uleb128(encdata, m_evalues->size());
  for (auto const& ev : *m_evalues) {
    ev->encode(dodx, encdata);
  }
}

void DexEncodedValueAnnotation::encode(DexOutputIdx* dodx, uint8_t*& encdata) {
  uint8_t devtb = DEVT_HDR_TYPE(m_evtype);
  uint32_t tidx = dodx->typeidx(m_type);
  *encdata++ = devtb;
  encdata = write_uleb128(encdata, tidx);
  encdata = write_uleb128(encdata, m_annotations->size());
  for (auto const& dae : *m_annotations) {
    DexString* str = dae.string;
    DexEncodedValue* dev = dae.encoded_value;
    uint32_t sidx = dodx->stringidx(str);
    encdata = write_uleb128(encdata, sidx);
    dev->encode(dodx, encdata);
  }
}

static DexAnnotationElement get_annotation_element(DexIdx* idx,
                                                   const uint8_t*& encdata) {
  uint32_t sidx = read_uleb128(&encdata);
  DexString* name = idx->get_stringidx(sidx);
  always_assert_log(name != nullptr,
                    "Invalid string idx in annotation element");
  DexEncodedValue* dev = DexEncodedValue::get_encoded_value(idx, encdata);
  return DexAnnotationElement(name, dev);
}

DexEncodedValueArray* get_encoded_value_array(DexIdx* idx,
                                              const uint8_t*& encdata) {
  uint32_t size = read_uleb128(&encdata);
  std::list<DexEncodedValue*>* evlist = new std::list<DexEncodedValue*>();
  for (uint32_t i = 0; i < size; i++) {
    DexEncodedValue* adev = DexEncodedValue::get_encoded_value(idx, encdata);
    evlist->push_back(adev);
  }
  return new DexEncodedValueArray(evlist);
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

DexEncodedValue* DexEncodedValue::get_encoded_value(DexIdx* idx,
                                                    const uint8_t*& encdata) {
  uint8_t evhdr = *encdata++;
  DexEncodedValueTypes evt = (DexEncodedValueTypes)DEVT_HDR_TYPE(evhdr);
  uint8_t evarg = DEVT_HDR_ARG(evhdr);
  switch (evt) {
  case DEVT_SHORT:
  case DEVT_INT:
  case DEVT_LONG: {
    uint64_t v = read_evarg(encdata, evarg, true /* sign_extend */);
    return new DexEncodedValue(evt, v);
  }
  case DEVT_BYTE:
  case DEVT_CHAR: {
    uint64_t v = read_evarg(encdata, evarg, false /* sign_extend */);
    return new DexEncodedValue(evt, v);
  }
  case DEVT_FLOAT: {
    uint64_t v = read_evarg(encdata, evarg, false) << ((3 - evarg) * 8);
    return new DexEncodedValue(evt, v);
  }
  case DEVT_DOUBLE: {
    uint64_t v = read_evarg(encdata, evarg, false) << ((7 - evarg) * 8);
    return new DexEncodedValue(evt, v);
  }
  case DEVT_NULL:
    return new DexEncodedValueBit(evt, false);
  case DEVT_BOOLEAN:
    return new DexEncodedValueBit(evt, evarg > 0);
  case DEVT_STRING: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexString* evstring = idx->get_stringidx(evidx);
    always_assert_log(evstring != nullptr,
                      "Invalid string idx in annotation element");
    return new DexEncodedValueString(evstring);
  }
  case DEVT_TYPE: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexType* evtype = idx->get_typeidx(evidx);
    always_assert_log(evtype != nullptr,
                      "Invalid type idx in annotation element");
    return new DexEncodedValueType(evtype);
  }
  case DEVT_FIELD:
  case DEVT_ENUM: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexField* evfield = idx->get_fieldidx(evidx);
    always_assert_log(evfield != nullptr,
                      "Invalid field idx in annotation element");
    return new DexEncodedValueField(evt, evfield);
  }
  case DEVT_METHOD: {
    uint32_t evidx = (uint32_t)read_evarg(encdata, evarg);
    DexMethod* evmethod = idx->get_methodidx(evidx);
    always_assert_log(evmethod != nullptr,
                      "Invalid method idx in annotation element");
    return new DexEncodedValueMethod(evmethod);
  }
  case DEVT_ARRAY:
    return get_encoded_value_array(idx, encdata);
  case DEVT_ANNOTATION: {
    EncodedAnnotations* eanno = new EncodedAnnotations();
    uint32_t tidx = read_uleb128(&encdata);
    uint32_t count = read_uleb128(&encdata);
    DexType* type = idx->get_typeidx(tidx);
    always_assert_log(type != nullptr,
                      "Invalid DEVT_ANNOTATION within annotation type");
    for (uint32_t i = 0; i < count; i++) {
      DexAnnotationElement dae = get_annotation_element(idx, encdata);
      eanno->push_back(dae);
    }
    return new DexEncodedValueAnnotation(type, eanno);
  }
  };
  always_assert_log(false, "Bogus annotation");
}

DexAnnotation* DexAnnotation::get_annotation(DexIdx* idx, uint32_t anno_off) {
  if (anno_off == 0) return nullptr;
  DexAnnotation* anno = new DexAnnotation();
  const uint8_t* encdata = idx->get_uleb_data(anno_off);
  uint8_t viz = *encdata++;
  always_assert_log(viz <= DAV_SYSTEM, "Invalid annotation visibility %d", viz);
  anno->m_viz = (DexAnnotationVisibility)viz;
  uint32_t tidx = read_uleb128(&encdata);
  uint32_t count = read_uleb128(&encdata);
  DexType* type = idx->get_typeidx(tidx);
  always_assert_log(type != nullptr, "Invalid annotation type");
  anno->m_type = type;
  for (uint32_t i = 0; i < count; i++) {
    DexAnnotationElement dae = get_annotation_element(idx, encdata);
    anno->m_anno_elems.push_back(dae);
  }
  return anno;
}

DexAnnotationSet* DexAnnotationSet::get_annotation_set(DexIdx* idx,
                                                       uint32_t aset_off) {
  if (aset_off == 0) return nullptr;
  const uint32_t* adata = idx->get_uint_data(aset_off);
  DexAnnotationSet* aset = new DexAnnotationSet();
  uint32_t count = *adata++;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t off = adata[i];
    DexAnnotation* anno = DexAnnotation::get_annotation(idx, off);
    if (anno != nullptr) {
      aset->m_annotations.push_back(anno);
    }
  }
  return aset;
}

void DexAnnotationDirectory::calc_internals() {
  int cntviz = 0;
  if (m_class) {
    int ca, cv;
    m_class->viz_counts(ca, cv);
    m_aset_size += 4 + 4 * (ca);
    m_aset_count++;
    m_anno_count += ca;
    cntviz += cv;
  }
  if (m_field) {
    for (auto const& p : *m_field) {
      DexAnnotationSet* das = p.second;
      int ca, cv;
      das->viz_counts(ca, cv);
      m_anno_count += ca;
      m_aset_size += 4 + 4 * (ca);
      m_aset_count++;
      cntviz += cv;
    }
  }
  if (m_method) {
    for (auto const& p : *m_method) {
      DexAnnotationSet* das = p.second;
      int ca, cv;
      das->viz_counts(ca, cv);
      m_anno_count += ca;
      m_aset_size += 4 + 4 * (ca);
      m_aset_count++;
      cntviz += cv;
    }
  }
  if (m_method_param) {
    for (auto const& p : *m_method_param) {
      ParamAnnotations* pa = p.second;
      m_xref_size += 4 + 4 * pa->size();
      m_xref_count++;
      for (auto const& pp : *pa) {
        DexAnnotationSet* das = pp.second;
        int ca, cv;
        das->viz_counts(ca, cv);
        m_anno_count += ca;
        m_aset_size += 4 + 4 * (ca);
        m_aset_count++;
        cntviz += cv;
      }
    }
  }
  m_viz = (double)cntviz / m_anno_count;
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

bool field_annotation_compare(std::pair<DexField*, DexAnnotationSet*> a,
                              std::pair<DexField*, DexAnnotationSet*> b) {
  return compare_dexfields(a.first, b.first);
}

bool type_annotation_compare(DexAnnotation* a, DexAnnotation* b) {
  return compare_dextypes(a->type(), b->type());
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
      for (auto param : *params)
        aset.push_back(param.second);
    }
  }
}

void DexAnnotationDirectory::gather_xrefs(
    std::vector<ParamAnnotations*>& xrefs) {
  if (m_method_param) {
    m_method_param->sort(method_param_annotation_compare);
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
      for (auto param : *params) {
        DexAnnotationSet* das = param.second;
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
    always_assert_log(asetmap.count(m_class) != 0,
                      "Uninitialized aset %p '%s'",
                      m_class, show(m_class).c_str());
    classoff = asetmap[m_class];
  }
  if (m_field) {
    cntaf = m_field->size();
  }
  if (m_method) {
    cntam = m_method->size();
  }
  if (m_method_param) {
    cntamp = m_method_param->size();
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
    m_field->sort(field_annotation_compare);
    for (auto const& p : *m_field) {
      DexAnnotationSet* das = p.second;
      annodirout.push_back(dodx->fieldidx(p.first));
      always_assert_log(asetmap.count(das) != 0,
                        "Uninitialized aset %p '%s'",
                        das, show(das).c_str());
      annodirout.push_back(asetmap[das]);
    }
  }
  if (m_method) {
    m_method->sort(method_annotation_compare);
    for (auto const& p : *m_method) {
      DexMethod* m = p.first;
      DexAnnotationSet* das = p.second;
      uint32_t midx = dodx->methodidx(m);
      annodirout.push_back(midx);
      always_assert_log(asetmap.count(das) != 0,
                        "Uninitialized aset %p '%s'",
                        das, show(das).c_str());
      annodirout.push_back(asetmap[das]);
    }
  }
  if (m_method_param) {
    m_method_param->sort(method_param_annotation_compare);
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
  for (auto annotation : m_annotations) {
    list.push_back(annotation);
  }
}

void DexAnnotationSet::vencode(DexOutputIdx* dodx,
                               std::vector<uint32_t>& asetout,
                               std::map<DexAnnotation*, uint32_t>& annoout) {
  asetout.push_back(m_annotations.size());
  m_annotations.sort(type_annotation_compare);
  for (auto anno : m_annotations) {
    always_assert_log(annoout.count(anno) != 0,
                      "Uninitialized annotation %p '%s', bailing\n",
                      anno, show(anno).c_str());
    asetout.push_back(annoout[anno]);
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
  uleb_append(bytes, m_anno_elems.size());
  for (auto elem : m_anno_elems) {
    DexString* string = elem.string;
    DexEncodedValue* ev = elem.encoded_value;
    uleb_append(bytes, dodx->stringidx(string));
    ev->vencode(dodx, bytes);
  }
}
