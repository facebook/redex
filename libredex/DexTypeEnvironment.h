/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iosfwd>

#include <boost/optional.hpp>

#include <sparta/AbstractDomain.h>
#include <sparta/FiniteAbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>
#include <sparta/PatriciaTreeSet.h>
#include <sparta/ReducedProductAbstractDomain.h>

#include "DexUtil.h"
#include "NullnessDomain.h"
#include "TypeUtil.h"

namespace dtv_impl {

class DexTypeValue final : public sparta::AbstractValue<DexTypeValue> {
 public:
  ~DexTypeValue() {
    static_assert(std::is_default_constructible<DexType*>::value,
                  "Constant is not default constructible");
    static_assert(std::is_copy_constructible<DexType*>::value,
                  "Constant is not copy constructible");
    static_assert(std::is_copy_assignable<DexType*>::value,
                  "Constant is not copy assignable");
  }

  DexTypeValue() = default;

  explicit DexTypeValue(const DexType* dex_type) : m_dex_type(dex_type) {}

  void clear() { m_dex_type = nullptr; }

  sparta::AbstractValueKind kind() const {
    return sparta::AbstractValueKind::Value;
  }

  /*
   * None means there's no type value. It can be used to denote a null in Java
   * or the type of an uninitialized Java field. It is conceptually similar to
   * Bottom but not an actual Bottom in an AbstractDomain.
   *
   * The reason we need this special case is because in Sparta, we cannot assign
   * a Bottom to an Environment or a ReduceProductAbstractDomain. Doing so will
   * mark the entire thing as Bottom. A Bottom environment or domain carries a
   * different meaning in the analysis. Therefore, we need something that is not
   * a Bottom to denote an empty or uninitialized DexType value.
   *
   * It is not necessarily the case in a different DexTypeDomain implementation.
   * If we as an alternative model the DexTypeDomain as a set of DexTypes, we
   * can use an empty set to denote a none.
   */
  bool is_none() const { return m_dex_type == nullptr; }

  bool leq(const DexTypeValue& other) const {
    if (equals(other)) {
      return true;
    }
    if (is_none()) {
      return true;
    }
    if (other.is_none()) {
      return false;
    }
    auto l = get_dex_type();
    auto r = other.get_dex_type();
    return type::check_cast(l, r);
  }

  bool equals(const DexTypeValue& other) const {
    return m_dex_type == other.get_dex_type();
  }

  sparta::AbstractValueKind join_with(const DexTypeValue& other);

  sparta::AbstractValueKind widen_with(const DexTypeValue& other) {
    if (equals(other)) {
      return kind();
    }
    if (is_none()) {
      m_dex_type = other.get_dex_type();
      return sparta::AbstractValueKind::Value;
    } else if (other.is_none()) {
      return sparta::AbstractValueKind::Value;
    }
    // Converge to Top earlier than join_with.
    clear();
    return sparta::AbstractValueKind::Top;
  }

  sparta::AbstractValueKind meet_with(const DexTypeValue& other) {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }
    clear();
    return sparta::AbstractValueKind::Bottom;
  }

  sparta::AbstractValueKind narrow_with(const DexTypeValue& other) {
    return meet_with(other);
  }

  const DexType* get_dex_type() const { return m_dex_type; }

 private:
  const DexType* m_dex_type;
};

} // namespace dtv_impl

/*
 * DexType domain
 *
 * Singleton here means that we only track a single DexType value. The join of
 * two distinct SingletonDexTypeDomain will produce a single DexType value that
 * is guaranteed to be compatible with the two inputs. This is the most simple
 * data structure we can use to represent a DexType domain.
 */
class SingletonDexTypeDomain final
    : public sparta::AbstractDomainScaffolding<dtv_impl::DexTypeValue,
                                               SingletonDexTypeDomain> {
 public:
  SingletonDexTypeDomain() { this->set_to_top(); }

  explicit SingletonDexTypeDomain(const DexType* cst) {
    this->set_to_value(dtv_impl::DexTypeValue(cst));
  }

  explicit SingletonDexTypeDomain(sparta::AbstractValueKind kind)
      : sparta::AbstractDomainScaffolding<dtv_impl::DexTypeValue,
                                          SingletonDexTypeDomain>(kind) {}

  boost::optional<const DexType*> get_dex_type() const {
    if (this->kind() != sparta::AbstractValueKind::Value || this->is_none()) {
      return boost::none;
    }
    return boost::optional<const DexType*>(this->get_value()->get_dex_type());
  }

  static SingletonDexTypeDomain bottom() {
    return SingletonDexTypeDomain(sparta::AbstractValueKind::Bottom);
  }

  static SingletonDexTypeDomain top() {
    return SingletonDexTypeDomain(sparta::AbstractValueKind::Top);
  }

  static SingletonDexTypeDomain none() {
    return SingletonDexTypeDomain(nullptr);
  }

  bool is_none() const {
    return this->kind() == sparta::AbstractValueKind::Value &&
           this->get_value()->is_none();
  }

  friend std::ostream& operator<<(std::ostream& out,
                                  const SingletonDexTypeDomain& x);
};

std::ostream& operator<<(std::ostream& out, const SingletonDexTypeDomain& x);

std::ostream& operator<<(std::ostream& output, const DexType* dex_type);

/*
 *
 * Small Set DexTypeDomain
 *
 */
constexpr size_t MAX_SET_SIZE = 4;

class SmallSetDexTypeDomain final
    : public sparta::AbstractDomain<SmallSetDexTypeDomain> {
 public:
  SmallSetDexTypeDomain() : m_kind(sparta::AbstractValueKind::Value) {}

  explicit SmallSetDexTypeDomain(const DexType* type) {
    m_types.insert(type);
    m_kind = sparta::AbstractValueKind::Value;
  }

  bool is_bottom() const { return m_kind == sparta::AbstractValueKind::Bottom; }

  bool is_top() const { return m_kind == sparta::AbstractValueKind::Top; }

  void set_to_bottom() {
    m_kind = sparta::AbstractValueKind::Bottom;
    m_types.clear();
  }

  void set_to_top() {
    m_kind = sparta::AbstractValueKind::Top;
    m_types.clear();
  }

  sparta::AbstractValueKind kind() const { return m_kind; }

  const sparta::PatriciaTreeSet<const DexType*>& get_types() const {
    always_assert(!is_top());
    return m_types;
  }

  bool leq(const SmallSetDexTypeDomain& other) const;

  bool equals(const SmallSetDexTypeDomain& other) const;

  void join_with(const SmallSetDexTypeDomain& other);

  void widen_with(const SmallSetDexTypeDomain& other);

  void meet_with(const SmallSetDexTypeDomain& /* other */) {
    throw std::runtime_error("meet_with not implemented!");
  }

  void narrow_with(const SmallSetDexTypeDomain& /* other */) {
    throw std::runtime_error("narrow_with not implemented!");
  }

  friend std::ostream& operator<<(std::ostream& out,
                                  const SmallSetDexTypeDomain& x);

 private:
  sparta::PatriciaTreeSet<const DexType*> m_types;
  sparta::AbstractValueKind m_kind;
};

/*
 * Domain for StringDef and IntDef annotations, where the DexType will track the
 * annotation name. This will enforce null safety and prevent the joins of two
 * different annotations.
 * https://developer.android.com/studio/write/annotations#enum-annotations
 */
using TypedefAnnotationDomain = SingletonDexTypeDomain;

/*
 *
 * ArrayConstNullnessDomain X SingletonDexTypeDomain X SmallSetDexTypeDomain
 *
 *
 * When the SmallSetDexTypeDomain has elements, then they represent an exact set
 * of non-interface classes (including arrays), or possibly java.lang.Throwable.
 */
class DexTypeDomain final
    : public sparta::ReducedProductAbstractDomain<DexTypeDomain,
                                                  ArrayConstNullnessDomain,
                                                  SingletonDexTypeDomain,
                                                  SmallSetDexTypeDomain,
                                                  TypedefAnnotationDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  using BaseType =
      sparta::ReducedProductAbstractDomain<DexTypeDomain,
                                           ArrayConstNullnessDomain,
                                           SingletonDexTypeDomain,
                                           SmallSetDexTypeDomain,
                                           TypedefAnnotationDomain>;

  // Some older compilers complain that the class is not default
  // constructible. We intended to use the default constructors of the base
  // class (via the `using` declaration above), but some compilers fail to
  // catch this. So we insert a redundant '= default'.
  DexTypeDomain() = default;

  explicit DexTypeDomain(int64_t v)
      : ReducedProductAbstractDomain(
            std::make_tuple(ConstNullnessDomain(v),
                            SingletonDexTypeDomain(),
                            SmallSetDexTypeDomain::top(),
                            TypedefAnnotationDomain())) {}

  explicit DexTypeDomain(const DexType* array_type, uint32_t array_length)
      : ReducedProductAbstractDomain(
            std::make_tuple(ArrayNullnessDomain(array_length),
                            SingletonDexTypeDomain(array_type),
                            SmallSetDexTypeDomain(array_type),
                            TypedefAnnotationDomain())) {}

  explicit DexTypeDomain(const DexType* dex_type,
                         const DexType* annotation = nullptr)
      : ReducedProductAbstractDomain(
            std::make_tuple(ConstNullnessDomain(NOT_NULL),
                            SingletonDexTypeDomain(dex_type),
                            SmallSetDexTypeDomain(dex_type),
                            TypedefAnnotationDomain(annotation))) {}

  explicit DexTypeDomain(const DexType* dex_type,
                         const Nullness nullness,
                         bool is_dex_type_exact,
                         const DexType* annotation = nullptr)
      : ReducedProductAbstractDomain(
            std::make_tuple(ConstNullnessDomain(nullness),
                            SingletonDexTypeDomain(dex_type),
                            is_dex_type_exact ? SmallSetDexTypeDomain(dex_type)
                                              : SmallSetDexTypeDomain::top(),
                            TypedefAnnotationDomain(annotation))) {}

  static void reduce_product(
      std::tuple<ArrayConstNullnessDomain,
                 SingletonDexTypeDomain,
                 SmallSetDexTypeDomain,
                 TypedefAnnotationDomain>& /* product */) {}

  static DexTypeDomain null() { return DexTypeDomain(IS_NULL); }

  NullnessDomain get_nullness() const {
    auto domain = get<0>();
    if (domain.which() == 0) {
      return domain.get<ConstNullnessDomain>().get_nullness();
    } else {
      return domain.get<ArrayNullnessDomain>().get_nullness();
    }
  }

  bool is_null() const { return get_nullness().element() == IS_NULL; }

  bool is_not_null() const { return get_nullness().element() == NOT_NULL; }

  bool is_nullable() const { return get_nullness().is_top(); }

  boost::optional<ConstantDomain::ConstantType> get_constant() const {
    if (auto const_nullness = get<0>().maybe_get<ConstNullnessDomain>()) {
      return const_nullness->const_domain().get_constant();
    }
    return boost::none;
  }

  ArrayNullnessDomain get_array_nullness() const {
    if (auto array_nullness = get<0>().maybe_get<ArrayNullnessDomain>()) {
      return *array_nullness;
    }
    return ArrayNullnessDomain::top();
  }

  NullnessDomain get_array_element_nullness(
      boost::optional<int64_t> idx) const {
    if (!ArrayNullnessDomain::is_valid_array_idx(idx)) {
      return NullnessDomain::top();
    }
    return get_array_nullness().get_element(*idx);
  }

  void set_array_element_nullness(boost::optional<int64_t> idx,
                                  const NullnessDomain& nullness) {
    if (!ArrayNullnessDomain::is_valid_array_idx(idx)) {
      apply<0>([&](ArrayConstNullnessDomain* d) {
        d->apply<ArrayNullnessDomain>([&](ArrayNullnessDomain* array_nullness) {
          array_nullness->reset_elements();
        });
      });
      return;
    }
    apply<0>([&](ArrayConstNullnessDomain* d) {
      d->apply<ArrayNullnessDomain>([&](ArrayNullnessDomain* array_nullness) {
        array_nullness->set_element(*idx, nullness);
      });
    });
  }

  const SingletonDexTypeDomain& get_single_domain() const { return get<1>(); }

  const TypedefAnnotationDomain& get_annotation_domain() const {
    return get<3>();
  }

  boost::optional<const DexType*> get_dex_type() const {
    return get<1>().get_dex_type();
  }

  boost::optional<const DexType*> get_annotation_type() const {
    return get<3>().get_dex_type();
  }

  boost::optional<const DexClass*> get_dex_cls() const {
    auto dex_type = get<1>().get_dex_type();
    if (!dex_type) {
      return boost::none;
    }
    auto dex_cls = type_class(*dex_type);
    return dex_cls ? boost::optional<const DexClass*>(dex_cls) : boost::none;
  }

  const SmallSetDexTypeDomain& get_set_domain() const { return get<2>(); }

  const sparta::PatriciaTreeSet<const DexType*>& get_type_set() const {
    return get<2>().get_types();
  }

 private:
  explicit DexTypeDomain(const Nullness nullness)
      : ReducedProductAbstractDomain(
            std::make_tuple(ConstNullnessDomain(nullness),
                            SingletonDexTypeDomain::none(),
                            SmallSetDexTypeDomain(),
                            TypedefAnnotationDomain::none())) {}
};

/*
 * We model the register to DexTypeDomain mapping using an Environment. A
 * write to a register always overwrites the existing mapping.
 */
using RegTypeEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, DexTypeDomain>;

/*
 * We model the field to DexTypeDomain mapping using an Environment. But we
 * need to handle the initial write to a field correctly. We should overwrite
 * the default top value of a field upon the 1st write. The subsequent writes
 * to the same field always require a join with the existing type mapping to
 * preserve all the type information.
 *
 * Note that at method level, this field type mapping can still be incomplete.
 * We need to join all the mappings from the analysis for all methods globally
 * to make sure that we don't lose any type information for a given field.
 * However, we can always fall back to the declared type, which is still
 * sound.
 */
using FieldTypeEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<const DexField*, DexTypeDomain>;

/*
 * A simple Environment that tracks the registers possibly holding the value of
 * `this` pointer.
 *
 * The purpose of this is to make the FieldTypeEnvironment propagation in
 * CtorFieldAnalyzer instance sensitive. We can ignore field operations that do
 * not update field types on `this` obj.
 */
using IsDomain = sparta::ConstantAbstractDomain<bool>;
using ThisPointerEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, IsDomain>;

/*
 * Combining the register mapping and the field mapping to the DexTypeDomain.
 */
class DexTypeEnvironment final
    : public sparta::ReducedProductAbstractDomain<DexTypeEnvironment,
                                                  RegTypeEnvironment,
                                                  FieldTypeEnvironment,
                                                  ThisPointerEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default
  // constructible. We intended to use the default constructors of the base
  // class (via the `using` declaration above), but some compilers fail to
  // catch this. So we insert a redundant '= default'.
  DexTypeEnvironment() = default;

  DexTypeEnvironment(std::initializer_list<std::pair<reg_t, DexTypeDomain>> l)
      : ReducedProductAbstractDomain(
            std::make_tuple(RegTypeEnvironment(l),
                            FieldTypeEnvironment(),
                            ThisPointerEnvironment())) {}

  static void reduce_product(std::tuple<RegTypeEnvironment,
                                        FieldTypeEnvironment,
                                        ThisPointerEnvironment>&) {}

  /*
   * Getters and setters
   */
  const RegTypeEnvironment& get_reg_environment() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const FieldTypeEnvironment& get_field_environment() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  const ThisPointerEnvironment& get_this_ptr_environment() const {
    return ReducedProductAbstractDomain::get<2>();
  }

  DexTypeDomain get(reg_t reg) const { return get_reg_environment().get(reg); }

  DexTypeDomain get(DexField* field) const {
    return get_field_environment().get(field);
  }

  DexTypeEnvironment& mutate_reg_environment(
      const std::function<void(RegTypeEnvironment*)>& f) {
    apply<0>(f);
    return *this;
  }

  DexTypeEnvironment& mutate_field_environment(
      const std::function<void(FieldTypeEnvironment*)>& f) {
    apply<1>(f);
    return *this;
  }

  DexTypeEnvironment& set(reg_t reg, const DexTypeDomain& type) {
    return mutate_reg_environment(
        [&](RegTypeEnvironment* env) { env->set(reg, type); });
  }

  DexTypeEnvironment& set(const DexField* field, const DexTypeDomain& type) {
    return mutate_field_environment(
        [&](FieldTypeEnvironment* env) { env->set(field, type); });
  }

  DexTypeEnvironment& clear_field_environment() {
    return mutate_field_environment(
        [](FieldTypeEnvironment* env) { env->set_to_bottom(); });
  }

  bool is_this_ptr(reg_t reg) const {
    auto is_this = get_this_ptr_environment().get(reg).get_constant();
    return is_this && *is_this;
  }

  IsDomain get_this_ptr(reg_t reg) const {
    return get_this_ptr_environment().get(reg);
  }

  void set_this_ptr(reg_t reg, const IsDomain& is_this) {
    apply<2>([&](ThisPointerEnvironment* env) { env->set(reg, is_this); });
  }
};
