/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <list>
#include <map>
#include <sstream>

#include "Gatherable.h"
#include "Show.h"

class DexField;
class DexIdx;
class DexOutputIdx;
class DexString;
class DexType;

enum DexEncodedValueTypes : uint8_t {
  DEVT_BYTE =       0x00,
  DEVT_SHORT =      0x02,
  DEVT_CHAR =       0x03,
  DEVT_INT =        0x04,
  DEVT_LONG =       0x06,
  DEVT_FLOAT =      0x10,
  DEVT_DOUBLE =     0x11,
  DEVT_STRING =     0x17,
  DEVT_TYPE =       0x18,
  DEVT_FIELD =      0x19,
  DEVT_METHOD =     0x1a,
  DEVT_ENUM  =      0x1b,
  DEVT_ARRAY =      0x1c,
  DEVT_ANNOTATION = 0x1d,
  DEVT_NULL =       0x1e,
  DEVT_BOOLEAN =    0x1f
};

inline uint8_t DEVT_HDR_TYPE(uint8_t x) { return x & 0x1f; }
inline uint8_t DEVT_HDR_ARG(uint8_t x) { return (x >> 5) & 0x7; }
inline uint8_t TO_DEVT_HDR_ARG(uint8_t x) { return (x & 0x7) << 5; }

enum DexAnnotationVisibility : uint8_t {
  DAV_BUILD = 0,
  DAV_RUNTIME = 1,
  DAV_SYSTEM = 2,
};

class DexEncodedValue : public Gatherable {
 protected:
  DexEncodedValueTypes m_evtype;
  uint64_t m_value;

  DexEncodedValue(DexEncodedValueTypes type, uint64_t value = 0)
      : Gatherable() {
    m_evtype = type;
    m_value = value;
  }

 public:
  DexEncodedValueTypes evtype() const { return m_evtype; }
  bool is_evtype_primitive() const;
  uint64_t value() const { return m_value; }
  static DexEncodedValue* get_encoded_value(DexIdx* idx,
                                            const uint8_t*& encdata);
  DexEncodedValueTypes evtype() { return m_evtype; }
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);
  void vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes);

  virtual std::string show() const;
};

class DexEncodedValueBit : public DexEncodedValue {
 public:
  DexEncodedValueBit(DexEncodedValueTypes type, bool bit)
      : DexEncodedValue(type, bit) {}

  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);
};

class DexEncodedValueString : public DexEncodedValue {
  DexString* m_string;

 public:
  DexEncodedValueString(DexString* string) : DexEncodedValue(DEVT_STRING) {
    m_string = string;
  }

  DexString* string() const { return m_string; }
  void string(DexString *string) { m_string = string; }
  virtual void gather_strings(std::vector<DexString*>& lstring);
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);

  virtual std::string show() const { return ::show(m_string); }
};

class DexEncodedValueType : public DexEncodedValue {
  DexType* m_type;

 public:
  DexEncodedValueType(DexType* type) : DexEncodedValue(DEVT_TYPE) {
    m_type = type;
  }

  virtual void gather_types(std::vector<DexType*>& ltype);
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);

  DexType* type() const { return m_type; }
  void rewrite_type(DexType* type) { m_type = type; }
  virtual std::string show() const { return ::show(m_type); }
};

class DexEncodedValueField : public DexEncodedValue {
  DexField* m_field;

 public:
  DexEncodedValueField(DexEncodedValueTypes type, DexField* field)
      : DexEncodedValue(type) {
    m_field = field;
  }

  virtual void gather_fields(std::vector<DexField*>& lfield);
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);

  DexField* field() const { return m_field; }
  void rewrite_field(DexField* field) { m_field = field; }
  virtual std::string show() const { return ::show(m_field); }
};

class DexEncodedValueMethod : public DexEncodedValue {
  DexMethod* m_method;

 public:
  DexEncodedValueMethod(DexMethod* method) : DexEncodedValue(DEVT_METHOD) {
    m_method = method;
  }

  virtual void gather_methods(std::vector<DexMethod*>& lmethod);
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);

  DexMethod* method() const { return m_method; }
  void rewrite_method(DexMethod* method) { m_method = method; }
  virtual std::string show() const { return ::show(m_method); }
};

class DexEncodedValueArray : public DexEncodedValue {
  std::list<DexEncodedValue*>* m_evalues;
  bool m_static_val;

 public:
  /*
   * Static values are encoded without a DEVT_ARRAY header byte
   * so we differentiate that here.
   */
  DexEncodedValueArray(std::list<DexEncodedValue*>* evalues,
                       bool static_val = false)
      : DexEncodedValue(DEVT_ARRAY) {
    m_evalues = evalues;
    m_static_val = static_val;
  }

  ~DexEncodedValueArray() { delete m_evalues; }

  std::list<DexEncodedValue*>*& evalues() { return m_evalues; }

  DexEncodedValue* pop_next() {
    if (m_evalues->empty()) return nullptr;
    DexEncodedValue* rv = m_evalues->front();
    m_evalues->pop_front();
    return rv;
  }

  virtual void gather_types(std::vector<DexType*>& ltype);
  virtual void gather_fields(std::vector<DexField*>& lfield);
  virtual void gather_methods(std::vector<DexMethod*>& lmethod);
  virtual void gather_strings(std::vector<DexString*>& lstring);
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);

  virtual std::string show() const;
};

/* For loading static values */
DexEncodedValueArray* get_encoded_value_array(DexIdx* idx,
                                              const uint8_t*& encdata);

/*
 * These are not "full blown" annotations, they are
 * key/value pairs of encoded values.  They inherit
 * visibility from the referrer.  Preserving the odd
 * naming from the spec.  In practice, these are the
 * InnerClass annotations things like access flags
 * or defining method/class.
 */
class DexAnnotationElement {
 public:
  DexAnnotationElement(DexString* s, DexEncodedValue* ev)
    : string(s),
      encoded_value(ev)
  {}

  DexString* string;
  DexEncodedValue* encoded_value;
};

typedef std::list<DexAnnotationElement> EncodedAnnotations;

std::string show(const EncodedAnnotations*);

class DexEncodedValueAnnotation : public DexEncodedValue {
  DexType* m_type;
  EncodedAnnotations* m_annotations;

 public:
  DexEncodedValueAnnotation(DexType* type, EncodedAnnotations* annotations)
      : DexEncodedValue(DEVT_ANNOTATION) {
    m_type = type;
    m_annotations = annotations;
  }

  DexType* type() const { return m_type; }
  void rewrite_type(DexType* type) { m_type = type; }
  const EncodedAnnotations* annotations() { return m_annotations; }

  virtual void gather_types(std::vector<DexType*>& ltype);
  virtual void gather_fields(std::vector<DexField*>& lfield);
  virtual void gather_methods(std::vector<DexMethod*>& lmethod);
  virtual void gather_strings(std::vector<DexString*>& lstring);
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);

  virtual std::string show() const;
};

class DexAnnotation : public Gatherable {
  EncodedAnnotations m_anno_elems;
  DexType* m_type;
  DexAnnotationVisibility m_viz;

  DexAnnotation() : Gatherable() {}

 public:
  static DexAnnotation* get_annotation(DexIdx* idx, uint32_t anno_off);
  virtual void gather_types(std::vector<DexType*>& ltype);
  virtual void gather_fields(std::vector<DexField*>& lfield);
  virtual void gather_methods(std::vector<DexMethod*>& lmethod);
  virtual void gather_strings(std::vector<DexString*>& lstring);

  bool runtime_visible() const {
    if (m_viz == DAV_RUNTIME) return true;
    return false;
  }

  bool build_visible() const {
    if (m_viz == DAV_BUILD) return true;
    return false;
  }

  bool system_visible() const {
    if (m_viz == DAV_SYSTEM) return true;
    return false;
  }

  void vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes);
  DexType* type() const { return m_type; }
  void rewrite_type(DexType* type) { m_type = type; }
  const EncodedAnnotations& anno_elems() { return m_anno_elems; }

  friend std::string show(const DexAnnotation*);
};

class DexAnnotationSet : public Gatherable {
  std::list<DexAnnotation*> m_annotations;

  DexAnnotationSet() : Gatherable() {}

 public:
  virtual void gather_types(std::vector<DexType*>& ltype);
  virtual void gather_fields(std::vector<DexField*>& lfield);
  virtual void gather_methods(std::vector<DexMethod*>& lmethod);
  virtual void gather_strings(std::vector<DexString*>& lstring);
  static DexAnnotationSet* get_annotation_set(DexIdx* idx, uint32_t aset_off);
  int size() { return m_annotations.size(); }
  void viz_counts(int& cntanno, int& cntviz) {
    cntanno = m_annotations.size();
    cntviz = 0;
    for (auto const& da : m_annotations) {
      if (da->runtime_visible()) cntviz++;
    }
  }

  const std::list<DexAnnotation*>& get_annotations() const {
    return m_annotations;
  }
  std::list<DexAnnotation*>& get_annotations() { return m_annotations; }
  void vencode(DexOutputIdx* dodx,
               std::vector<uint32_t>& asetout,
               std::map<DexAnnotation*, uint32_t>& annoout);
  void gather_annotations(std::vector<DexAnnotation*>& alist);
  friend std::string show(const DexAnnotationSet*);
};

typedef std::map<int, DexAnnotationSet*> ParamAnnotations;
typedef std::list<std::pair<DexField*, DexAnnotationSet*>> DexFieldAnnotations;
typedef std::list<std::pair<DexMethod*, DexAnnotationSet*>>
    DexMethodAnnotations;
typedef std::list<std::pair<DexMethod*, ParamAnnotations*>>
    DexMethodParamAnnotations;

class DexAnnotationDirectory {
  double m_viz;
  DexAnnotationSet* m_class;
  DexFieldAnnotations* m_field;
  DexMethodAnnotations* m_method;
  DexMethodParamAnnotations* m_method_param;
  int m_aset_size;
  int m_xref_size;
  int m_anno_count;
  int m_aset_count;
  int m_xref_count;
  void calc_internals();

 public:
  DexAnnotationDirectory(DexAnnotationSet* c,
                         DexFieldAnnotations* f,
                         DexMethodAnnotations* m,
                         DexMethodParamAnnotations* mp) {
    m_class = c;
    m_field = f;
    m_method = m;
    m_method_param = mp;
    m_aset_size = 0;
    m_xref_size = 0;
    m_anno_count = 0;
    m_aset_count = 0;
    m_xref_count = 0;
    calc_internals();
  }

  ~DexAnnotationDirectory() {
    delete m_class;
    delete m_field;
    delete m_method;
    delete m_method_param;
  }

  double viz_score() const { return m_viz; }

  /* Encoded sizes */
  int aset_size() { return m_aset_size; }
  int xref_size() { return m_xref_size; }

  int annodir_size() {
    int size = 4 * sizeof(uint32_t);
    if (m_field) {
      size += m_field->size() * 2 * sizeof(uint32_t);
    }
    if (m_method) {
      size += m_method->size() * 2 * sizeof(uint32_t);
    }
    if (m_method_param) {
      size += m_method_param->size() * 2 * sizeof(uint32_t);
    }
    return size;
  }

  int aset_count() { return m_aset_count; }
  int anno_count() { return m_anno_count; }
  int xref_count() { return m_xref_count; }
  void gather_annotations(std::vector<DexAnnotation*>& alist);
  void gather_asets(std::vector<DexAnnotationSet*>& aset);
  void gather_xrefs(std::vector<ParamAnnotations*>& xrefs);
  void vencode(DexOutputIdx* dodx,
               std::vector<uint32_t>& annodirout,
               std::map<ParamAnnotations*, uint32_t>& xrefmap,
               std::map<DexAnnotationSet*, uint32_t>& asetmap);

  friend std::string show(const DexAnnotationDirectory*);
};

uint64_t read_evarg(
  const uint8_t*& encdata,
  uint8_t evarg,
  bool sign_extend = false);

void type_encoder(uint8_t*& encdata, uint8_t type, uint64_t val);
void type_encoder_signext(uint8_t*& encdata, uint8_t type, uint64_t val);
void type_encoder_fp(uint8_t*& encdata, uint8_t type, uint64_t val);
