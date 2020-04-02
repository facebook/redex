/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ostream>

#include <boost/optional.hpp>

#include "AbstractDomain.h"
#include "DexUtil.h"
#include "FiniteAbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSet.h"
#include "ReducedProductAbstractDomain.h"

namespace dtv_impl {

class DexTypeValue final : public sparta::AbstractValue<DexTypeValue> {
 public:
  ~DexTypeValue() override {
    static_assert(std::is_default_constructible<DexType*>::value,
                  "Constant is not default constructible");
    static_assert(std::is_copy_constructible<DexType*>::value,
                  "Constant is not copy constructible");
    static_assert(std::is_copy_assignable<DexType*>::value,
                  "Constant is not copy assignable");
  }

  DexTypeValue() = default;

  explicit DexTypeValue(const DexType* dex_type) : m_dex_type(dex_type) {}

  void clear() override { m_dex_type = nullptr; }

  sparta::AbstractValueKind kind() const override {
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

  bool leq(const DexTypeValue& other) const override { return equals(other); }

  bool equals(const DexTypeValue& other) const override {
    return m_dex_type == other.get_dex_type();
  }

  sparta::AbstractValueKind join_with(const DexTypeValue& other) override;

  sparta::AbstractValueKind widen_with(const DexTypeValue& other) override {
    return join_with(other);
  }

  sparta::AbstractValueKind meet_with(const DexTypeValue& other) override {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }
    clear();
    return sparta::AbstractValueKind::Bottom;
  }

  sparta::AbstractValueKind narrow_with(const DexTypeValue& other) override {
    return meet_with(other);
  }

  const DexType* get_dex_type() const { return m_dex_type; }

 private:
  const DexType* m_dex_type;
};

} // namespace dtv_impl

enum Nullness {
  NN_BOTTOM,
  IS_NULL,
  NOT_NULL,
  NN_TOP // Nullable
};

using NullnessLattice = sparta::BitVectorLattice<Nullness, 4, std::hash<int>>;

/*
 *         TOP (Nullable)
 *        /      \
 *      NULL    NOT_NULL
 *        \      /
 *         BOTTOM
 */
extern NullnessLattice lattice;

/*
 * Nullness domain
 *
 * We can use the nullness domain to track the nullness of a given reference
 * type value.
 */
using NullnessDomain = sparta::FiniteAbstractDomain<Nullness,
                                                    NullnessLattice,
                                                    NullnessLattice::Encoding,
                                                    &lattice>;

std::ostream& operator<<(std::ostream& output, const Nullness& nullness);

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
                                  const SingletonDexTypeDomain& x) {
    using namespace sparta;
    switch (x.kind()) {
    case AbstractValueKind::Bottom: {
      out << "_|_";
      break;
    }
    case AbstractValueKind::Top: {
      out << "T";
      break;
    }
    case AbstractValueKind::Value: {
      auto type = x.get_dex_type();
      out << (type ? show(*type) : std::string("<NONE>"));
      break;
    }
    }
    return out;
  }
};

std::ostream& operator<<(std::ostream& output, const DexType* dex_type);

/*
 *
 * Small Set DexTypeDomain
 *
 */
enum class DexTypeValueKind { Bottom, SetValue, SingleValue, Top };

constexpr size_t MAX_SET_SIZE = 4;

class SmallSetDexTypeDomain final
    : public sparta::AbstractDomain<SmallSetDexTypeDomain> {
 public:
  SmallSetDexTypeDomain() : m_kind(DexTypeValueKind::Top) {}

  explicit SmallSetDexTypeDomain(const DexType* type) {
    m_types.insert(type);
    m_kind = DexTypeValueKind::SetValue;
  }

  bool is_bottom() const override { return m_kind == DexTypeValueKind::Bottom; }

  bool is_set_value() const { return m_kind == DexTypeValueKind::SetValue; }

  bool is_single_value() const {
    return m_kind == DexTypeValueKind::SingleValue;
  }

  bool is_top() const override { return m_kind == DexTypeValueKind::Top; }

  void set_to_bottom() override {
    m_kind = DexTypeValueKind::Bottom;
    m_types.clear();
  }

  void set_to_top() override {
    m_kind = DexTypeValueKind::Top;
    m_types.clear();
  }

  boost::optional<const DexType*> get_single_type() const {
    if (this->kind() != DexTypeValueKind::SingleValue ||
        m_single_type.get_dex_type() == nullptr) {
      return boost::none;
    }
    return boost::optional<const DexType*>(m_single_type.get_dex_type());
  }

  sparta::PatriciaTreeSet<const DexType*> get_types() const { return m_types; }

  DexTypeValueKind kind() const { return m_kind; }

  bool leq(const SmallSetDexTypeDomain& other) const override {
    if (is_bottom()) {
      return true;
    }
    if (other.is_bottom()) {
      return false;
    }
    if (other.is_top()) {
      return true;
    }
    if (is_top()) {
      return false;
    }
    if (other.is_single_value()) {
      return is_set_value();
    }
    if (is_single_value()) {
      // We don't do more precise comparison between single values.
      return other.is_single_value();
    }
    always_assert(this->is_set_value() && other.is_set_value());
    return m_types.is_subset_of(other.m_types);
  }

  bool equals(const SmallSetDexTypeDomain& other) const override {
    if (is_bottom()) {
      return other.is_bottom();
    }
    if (is_top()) {
      return other.is_top();
    }
    if (is_single_value() && other.is_single_value()) {
      return m_single_type.equals(other.m_single_type);
    }
    if (is_single_value() || other.is_single_value()) {
      return false;
    }
    return m_types.equals(other.m_types);
  }

  void join_with(const SmallSetDexTypeDomain& other) override;

  void widen_with(const SmallSetDexTypeDomain& other) override {
    join_with(other);
  }

  void meet_with(const SmallSetDexTypeDomain& /* other */) override {
    throw std::runtime_error("meet_with not implemented!");
  }

  void narrow_with(const SmallSetDexTypeDomain& /* other */) override {
    throw std::runtime_error("narrow_with not implemented!");
  }

 private:
  dtv_impl::DexTypeValue merge_to_single_val(
      const sparta::PatriciaTreeSet<const DexType*>& types);

  dtv_impl::DexTypeValue m_single_type;
  sparta::PatriciaTreeSet<const DexType*> m_types;
  DexTypeValueKind m_kind;
};

/*
 *
 * NullnessDomain X SingletonDexTypeDomain
 *
 */
class DexTypeDomain
    : public sparta::ReducedProductAbstractDomain<DexTypeDomain,
                                                  NullnessDomain,
                                                  SingletonDexTypeDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default
  // constructible. We intended to use the default constructors of the base
  // class (via the `using` declaration above), but some compilers fail to
  // catch this. So we insert a redundant '= default'.
  DexTypeDomain() = default;

  explicit DexTypeDomain(const DexType* dex_type)
      : ReducedProductAbstractDomain(std::make_tuple(
            NullnessDomain(NOT_NULL), SingletonDexTypeDomain(dex_type))) {}

  static void reduce_product(
      std::tuple<NullnessDomain, SingletonDexTypeDomain>& /* product */) {}

  static DexTypeDomain null() { return DexTypeDomain(IS_NULL); }

  bool is_null() const { return get<0>().element() == IS_NULL; }

  bool is_not_null() const { return get<0>().element() == NOT_NULL; }

  bool is_nullable() const { return get<0>().is_top(); }

  SingletonDexTypeDomain get_type_domain() { return get<1>(); }

  boost::optional<const DexType*> get_dex_type() const {
    return get<1>().get_dex_type();
  }

 private:
  explicit DexTypeDomain(const Nullness nullness)
      : ReducedProductAbstractDomain(std::make_tuple(
            NullnessDomain(nullness), SingletonDexTypeDomain::none())) {}
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
 * Combining the register mapping and the field mapping to the DexTypeDomain.
 */
class DexTypeEnvironment final
    : public sparta::ReducedProductAbstractDomain<DexTypeEnvironment,
                                                  RegTypeEnvironment,
                                                  FieldTypeEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // Some older compilers complain that the class is not default
  // constructible. We intended to use the default constructors of the base
  // class (via the `using` declaration above), but some compilers fail to
  // catch this. So we insert a redundant '= default'.
  DexTypeEnvironment() = default;

  DexTypeEnvironment(std::initializer_list<std::pair<reg_t, DexTypeDomain>> l)
      : ReducedProductAbstractDomain(
            std::make_tuple(RegTypeEnvironment(l), FieldTypeEnvironment())) {}

  static void reduce_product(
      std::tuple<RegTypeEnvironment, FieldTypeEnvironment>&) {}

  /*
   * Getters and setters
   */
  const RegTypeEnvironment& get_reg_environment() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const FieldTypeEnvironment& get_field_environment() const {
    return ReducedProductAbstractDomain::get<1>();
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
};
