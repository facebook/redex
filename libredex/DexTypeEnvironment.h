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
#include "PatriciaTreeMapAbstractEnvironment.h"
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

  void clear() override {}

  sparta::AbstractValueKind kind() const override {
    return sparta::AbstractValueKind::Value;
  }

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

class DexTypeDomain final
    : public sparta::AbstractDomainScaffolding<dtv_impl::DexTypeValue,
                                               DexTypeDomain> {
 public:
  DexTypeDomain() { this->set_to_top(); }

  explicit DexTypeDomain(const DexType* cst) {
    this->set_to_value(dtv_impl::DexTypeValue(cst));
  }

  explicit DexTypeDomain(sparta::AbstractValueKind kind)
      : sparta::AbstractDomainScaffolding<dtv_impl::DexTypeValue,
                                          DexTypeDomain>(kind) {}

  boost::optional<const DexType*> get_dex_type() const {
    return (this->kind() == sparta::AbstractValueKind::Value)
               ? boost::optional<const DexType*>(
                     this->get_value()->get_dex_type())
               : boost::none;
  }

  static DexTypeDomain bottom() {
    return DexTypeDomain(sparta::AbstractValueKind::Bottom);
  }

  static DexTypeDomain top() {
    return DexTypeDomain(sparta::AbstractValueKind::Top);
  }

  friend std::ostream& operator<<(std::ostream& out, const DexTypeDomain& x) {
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
 * We model the register to DexTypeDomain mapping using an Environment. A write
 * to a register always overwrites the existing mapping.
 */
using RegTypeEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, DexTypeDomain>;

/*
 * We model the field to DexTypeDomain mapping using an Environment. But we need
 * to handle the initial write to a field correctly. We should overwrite the
 * default top value of a field upon the 1st write. The subsequent writes to the
 * same field always require a join with the existing type mapping to preserve
 * all the type information.
 *
 * Note that at method level, this field type mapping can still be incomplete.
 * We need to join all the mappings from the analysis for all methods globally
 * to make sure that we don't lose any type information for a given field.
 * However, we can always fall back to the declared type, which is still sound.
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

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via the
  // `using` declaration above), but some compilers fail to catch this. So we
  // insert a redundant '= default'.
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
