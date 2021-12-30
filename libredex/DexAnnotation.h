/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <deque>
#include <map>
#include <unordered_set>
#include <vector>

#include "Gatherable.h"

class DexFieldRef;
class DexIdx;
class DexMethod;
class DexMethodRef;
class DexOutputIdx;
class DexString;
class DexType;

enum DexEncodedValueTypes : uint8_t {
  DEVT_BYTE = 0x00,
  DEVT_SHORT = 0x02,
  DEVT_CHAR = 0x03,
  DEVT_INT = 0x04,
  DEVT_LONG = 0x06,
  DEVT_FLOAT = 0x10,
  DEVT_DOUBLE = 0x11,
  DEVT_METHOD_TYPE = 0x15,
  DEVT_METHOD_HANDLE = 0x16,
  DEVT_STRING = 0x17,
  DEVT_TYPE = 0x18,
  DEVT_FIELD = 0x19,
  DEVT_METHOD = 0x1a,
  DEVT_ENUM = 0x1b,
  DEVT_ARRAY = 0x1c,
  DEVT_ANNOTATION = 0x1d,
  DEVT_NULL = 0x1e,
  DEVT_BOOLEAN = 0x1f
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
  union {
    uint64_t m_value;
    void* m_value_ptr;
    const void* m_value_ptr_const;
  };
  DexEncodedValueTypes m_evtype;

  explicit DexEncodedValue(DexEncodedValueTypes type, uint64_t value = 0)
      : m_value(value), m_evtype(type) {}

  DexEncodedValue(DexEncodedValueTypes type, void* value_ptr)
      : m_value_ptr(value_ptr), m_evtype(type) {}

  DexEncodedValue(DexEncodedValueTypes type, const void* value_ptr)
      : m_value_ptr_const(value_ptr), m_evtype(type) {}

 public:
  DexEncodedValueTypes evtype() const { return m_evtype; }
  bool is_evtype_primitive() const;
  void value(uint64_t value) { m_value = value; }
  uint64_t value() const { return m_value; }
  static DexEncodedValue* get_encoded_value(DexIdx* idx,
                                            const uint8_t*& encdata);
  DexEncodedValueTypes evtype() { return m_evtype; }
  virtual void encode(DexOutputIdx* dodx, uint8_t*& encdata);
  void vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes);

  virtual std::string show() const;
  virtual std::string show_deobfuscated() const { return show(); }
  virtual bool operator==(const DexEncodedValue& that) const {
    return m_evtype == that.m_evtype && m_value == that.m_value;
  }
  virtual bool operator!=(const DexEncodedValue& that) const {
    return !(*this == that);
  }
  virtual size_t hash_value() const {
    size_t seed = boost::hash<uint8_t>()(m_evtype);
    boost::hash_combine(seed, m_value);
    return seed;
  }

  bool is_zero() const;
  bool is_wide() const;
  static DexEncodedValue* zero_for_type(DexType* type);
};

inline size_t hash_value(const DexEncodedValue& v) { return v.hash_value(); }

class DexEncodedValueBit : public DexEncodedValue {
 public:
  DexEncodedValueBit(DexEncodedValueTypes type, bool bit)
      : DexEncodedValue(type, bit) {}

  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;
};

class DexEncodedValueString : public DexEncodedValue {
 public:
  explicit DexEncodedValueString(const DexString* string)
      : DexEncodedValue(DEVT_STRING, string) {}

  const DexString* string() const {
    return (const DexString*)m_value_ptr_const;
  }
  void string(const DexString* string) { m_value_ptr_const = string; }
  void gather_strings(std::vector<const DexString*>& lstring) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  std::string show() const override;
  bool operator==(const DexEncodedValue& that) const override {
    if (m_evtype != that.evtype()) {
      return false;
    }
    return m_value_ptr_const ==
           static_cast<const DexEncodedValueString*>(&that)->m_value_ptr_const;
  }
  size_t hash_value() const override {
    size_t seed = boost::hash<uint8_t>()(m_evtype);
    boost::hash_combine(seed, (uintptr_t)m_value_ptr_const);
    return seed;
  }
};

class DexEncodedValuePtr : public DexEncodedValue {
 protected:
  DexEncodedValuePtr(DexEncodedValueTypes type, void* data)
      : DexEncodedValue(type, data) {}

 public:
  bool operator==(const DexEncodedValue& that) const override {
    if (m_evtype != that.evtype()) {
      return false;
    }
    return m_value_ptr_const ==
           static_cast<const DexEncodedValuePtr*>(&that)->m_value_ptr;
  }
  size_t hash_value() const override {
    size_t seed = boost::hash<uint8_t>()(m_evtype);
    boost::hash_combine(seed, (uintptr_t)m_value_ptr);
    return seed;
  }
};

class DexEncodedValueType : public DexEncodedValuePtr {
 public:
  explicit DexEncodedValueType(DexType* type)
      : DexEncodedValuePtr(DEVT_TYPE, type) {}

  void gather_types(std::vector<DexType*>& ltype) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  DexType* type() const { return (DexType*)m_value_ptr; }
  void set_type(DexType* type) { m_value_ptr = type; }
  std::string show() const override;
};

class DexEncodedValueField : public DexEncodedValuePtr {
 public:
  DexEncodedValueField(DexEncodedValueTypes type, DexFieldRef* field)
      : DexEncodedValuePtr(type, field) {}

  void gather_fields(std::vector<DexFieldRef*>& lfield) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  DexFieldRef* field() const { return (DexFieldRef*)m_value_ptr; }
  void set_field(DexFieldRef* field) { m_value_ptr = field; }
  std::string show() const override;
  std::string show_deobfuscated() const override;
};

class DexEncodedValueMethod : public DexEncodedValuePtr {
 public:
  explicit DexEncodedValueMethod(DexMethodRef* method)
      : DexEncodedValuePtr(DEVT_METHOD, method) {}

  void gather_methods(std::vector<DexMethodRef*>& lmethod) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  DexMethodRef* method() const { return (DexMethodRef*)m_value_ptr; }
  void set_method(DexMethodRef* method) { m_value_ptr = method; }
  std::string show() const override;
  std::string show_deobfuscated() const override;
};

class DexEncodedValueMethodType : public DexEncodedValuePtr {
 public:
  explicit DexEncodedValueMethodType(DexProto* proto)
      : DexEncodedValuePtr(DEVT_METHOD_TYPE, proto) {}

  void gather_strings(std::vector<const DexString*>& lstring) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  DexProto* proto() const { return (DexProto*)m_value_ptr; }
  void set_proto(DexProto* proto) { m_value_ptr = proto; }
  std::string show() const override;
  std::string show_deobfuscated() const override;
};

class DexEncodedValueMethodHandle : public DexEncodedValuePtr {
 public:
  explicit DexEncodedValueMethodHandle(DexMethodHandle* methodhandle)
      : DexEncodedValuePtr(DEVT_METHOD_HANDLE, methodhandle) {}

  void gather_fields(std::vector<DexFieldRef*>& lfield) const override;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const override;
  void gather_methodhandles(
      std::vector<DexMethodHandle*>& lhandles) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  DexMethodHandle* methodhandle() const {
    return (DexMethodHandle*)m_value_ptr;
  }
  void set_methodhandle(DexMethodHandle* methodhandle) {
    m_value_ptr = methodhandle;
  }
  std::string show() const override;
  std::string show_deobfuscated() const override;
};

// NOTE: Different from the other values, this one owns the given vector.
class DexEncodedValueArray : public DexEncodedValue {
  bool m_static_val;

 public:
  /*
   * Static values are encoded without a DEVT_ARRAY header byte
   * so we differentiate that here.
   */
  explicit DexEncodedValueArray(std::vector<DexEncodedValue*>* evalues,
                                bool static_val = false)
      : DexEncodedValue(DEVT_ARRAY, evalues), m_static_val(static_val) {}
  ~DexEncodedValueArray() {
    delete (std::vector<DexEncodedValue*>*)m_value_ptr;
  }

  // May not copy or assign, as the element is owned.
  DexEncodedValueArray(const DexEncodedValueArray&) = delete;
  DexEncodedValueArray& operator=(const DexEncodedValueArray&) = delete;

  // May move.
  DexEncodedValueArray(DexEncodedValueArray&& rhs) noexcept
      : DexEncodedValue(DEVT_ARRAY, rhs.m_value_ptr),
        m_static_val(rhs.m_static_val) {
    rhs.m_value_ptr = nullptr;
  }
  DexEncodedValueArray& operator=(DexEncodedValueArray&& rhs) noexcept {
    delete (std::vector<DexEncodedValue*>*)m_value_ptr;
    m_value_ptr = rhs.m_value_ptr;
    m_static_val = rhs.m_static_val;
    rhs.m_value_ptr = nullptr;
    return *this;
  }

  std::vector<DexEncodedValue*>* evalues() const {
    return (std::vector<DexEncodedValue*>*)m_value_ptr;
  }
  bool is_static_val() const { return m_static_val; }

  void gather_types(std::vector<DexType*>& ltype) const override;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const override;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const override;
  void gather_strings(std::vector<const DexString*>& lstring) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  std::string show() const override;
  std::string show_deobfuscated() const override;

  bool operator==(const DexEncodedValueArray& that) const {
    if (evalues()->size() != that.evalues()->size()) {
      return false;
    }
    auto it = that.evalues()->begin();
    for (const auto& elem : *evalues()) {
      if (*elem != **it) {
        return false;
      }
      it = std::next(it);
    }
    return m_evtype == that.m_evtype && m_static_val == that.m_static_val;
  }

  size_t hash_value() const override {
    size_t seed = boost::hash<uint8_t>()(m_evtype);
    boost::hash_combine(seed, m_static_val);
    for (const auto& elem : *evalues()) {
      boost::hash_combine(seed, *elem);
    }
    return seed;
  }
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
  DexAnnotationElement(const DexString* s, DexEncodedValue* ev)
      : string(s), encoded_value(ev) {}

  const DexString* string;
  DexEncodedValue* encoded_value;
};

using EncodedAnnotations = std::vector<DexAnnotationElement>;

std::string show(const EncodedAnnotations*);
std::string show_deobfuscated(const EncodedAnnotations*);

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
  void set_type(DexType* type) { m_type = type; }
  const EncodedAnnotations* annotations() const { return m_annotations; }

  void gather_types(std::vector<DexType*>& ltype) const override;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const override;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const override;
  void gather_strings(std::vector<const DexString*>& lstring) const override;
  void encode(DexOutputIdx* dodx, uint8_t*& encdata) override;

  std::string show() const override;
  std::string show_deobfuscated() const override;
};

class DexAnnotation : public Gatherable {
  EncodedAnnotations m_anno_elems;
  DexType* m_type;
  DexAnnotationVisibility m_viz;

 public:
  DexAnnotation(DexType* type, DexAnnotationVisibility viz)
      : m_type(type), m_viz(viz) {}

  static DexAnnotation* get_annotation(DexIdx* idx, uint32_t anno_off);
  void gather_types(std::vector<DexType*>& ltype) const override;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const override;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const override;
  void gather_strings(std::vector<const DexString*>& lstring) const override;

  const EncodedAnnotations& anno_elems() const { return m_anno_elems; }
  void set_type(DexType* type) { m_type = type; }
  DexType* type() const { return m_type; }
  DexAnnotationVisibility viz() const { return m_viz; }
  bool runtime_visible() const { return m_viz == DAV_RUNTIME; }
  bool build_visible() const { return m_viz == DAV_BUILD; }
  bool system_visible() const { return m_viz == DAV_SYSTEM; }

  void vencode(DexOutputIdx* dodx, std::vector<uint8_t>& bytes);
  void add_element(const char* key, DexEncodedValue* value);
};

class DexAnnotationSet : public Gatherable {
  std::vector<DexAnnotation*> m_annotations;

 public:
  DexAnnotationSet() = default;
  DexAnnotationSet(const DexAnnotationSet& that) {
    for (const auto& anno : that.m_annotations) {
      m_annotations.push_back(new DexAnnotation(*anno));
    }
  }

  void gather_types(std::vector<DexType*>& ltype) const override;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const override;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const override;
  void gather_strings(std::vector<const DexString*>& lstring) const override;

  static DexAnnotationSet* get_annotation_set(DexIdx* idx, uint32_t aset_off);
  unsigned long size() const { return m_annotations.size(); }
  void viz_counts(unsigned long& cntanno, unsigned long& cntviz) {
    cntanno = m_annotations.size();
    cntviz = 0;
    for (auto const& da : m_annotations) {
      if (da->runtime_visible()) cntviz++;
    }
  }

  /**
   * Add in annotation missing from other annotation set.
   */
  void combine_with(const DexAnnotationSet& other) {
    std::unordered_set<DexType*> existing_annos_type;
    for (const auto existing_anno : m_annotations) {
      existing_annos_type.emplace(existing_anno->type());
    }
    auto const& other_annos = other.m_annotations;
    for (auto const& anno : other_annos) {
      if (existing_annos_type.count(anno->type()) == 0) {
        m_annotations.emplace_back(new DexAnnotation(*anno));
      }
    }
  }

  const std::vector<DexAnnotation*>& get_annotations() const {
    return m_annotations;
  }
  std::vector<DexAnnotation*>& get_annotations() { return m_annotations; }
  void add_annotation(DexAnnotation* anno) { m_annotations.emplace_back(anno); }
  void vencode(DexOutputIdx* dodx,
               std::vector<uint32_t>& asetout,
               std::map<DexAnnotation*, uint32_t>& annoout);
  void gather_annotations(std::vector<DexAnnotation*>& alist);
};

using ParamAnnotations = std::map<int, DexAnnotationSet*>;
using DexFieldAnnotations =
    std::vector<std::pair<DexFieldRef*, DexAnnotationSet*>>;
using DexMethodAnnotations =
    std::vector<std::pair<DexMethod*, DexAnnotationSet*>>;
using DexMethodParamAnnotations =
    std::vector<std::pair<DexMethod*, ParamAnnotations*>>;

class DexAnnotationDirectory {
  double m_viz;
  DexAnnotationSet* m_class;
  std::unique_ptr<DexFieldAnnotations> m_field;
  std::unique_ptr<DexMethodAnnotations> m_method;
  std::unique_ptr<DexMethodParamAnnotations> m_method_param;
  int m_aset_size;
  int m_xref_size;
  int m_anno_count;
  int m_aset_count;
  int m_xref_count;
  void calc_internals();

 public:
  DexAnnotationDirectory(DexAnnotationSet* c,
                         std::unique_ptr<DexFieldAnnotations> f,
                         std::unique_ptr<DexMethodAnnotations> m,
                         std::unique_ptr<DexMethodParamAnnotations> mp)
      : m_class(c),
        m_field(std::move(f)),
        m_method(std::move(m)),
        m_method_param(std::move(mp)),
        m_aset_size(0),
        m_xref_size(0),
        m_anno_count(0),
        m_aset_count(0),
        m_xref_count(0) {
    calc_internals();
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

uint64_t read_evarg(const uint8_t*& encdata,
                    uint8_t evarg,
                    bool sign_extend = false);

void type_encoder(uint8_t*& encdata, uint8_t type, uint64_t val);
void type_encoder_signext(uint8_t*& encdata, uint8_t type, uint64_t val);
void type_encoder_fp(uint8_t*& encdata, uint8_t type, uint64_t val);
