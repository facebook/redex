/**
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

  sparta::AbstractValueKind join_with(const DexTypeValue& other) override {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }

    // External classes. Cannot perform subclass check.
    const DexClass* this_cls =
        const_cast<const DexClass*>(type_class(get_dex_type()));
    const DexClass* other_cls =
        const_cast<const DexClass*>(type_class(other.get_dex_type()));
    if (!this_cls || !other_cls) {
      m_dex_type = get_object_type();
      return sparta::AbstractValueKind::Value;
    }
    // Direct subclass relation.
    if (is_subclass(get_dex_type(), other.get_dex_type())) {
      return sparta::AbstractValueKind::Value;
    } else if (is_subclass(other.get_dex_type(), get_dex_type())) {
      m_dex_type = other.get_dex_type();
      return sparta::AbstractValueKind::Value;
    }
    // Share common base type simple scenario.
    auto this_super_cls = this_cls->get_super_class();
    auto other_super_cls = other_cls->get_super_class();
    if (this_super_cls && is_subclass(this_super_cls, other.get_dex_type())) {
      m_dex_type = this_super_cls;
      return sparta::AbstractValueKind::Value;
    } else if (other_super_cls &&
               is_subclass(other_super_cls, get_dex_type())) {
      m_dex_type = other_super_cls;
      return sparta::AbstractValueKind::Value;
    }

    // Give up. Rewrite to java.lang.Object.
    m_dex_type = get_object_type();
    return sparta::AbstractValueKind::Value;
  }

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
      out << show(x.get_dex_type());
      break;
    }
    }
    return out;
  }
};

std::ostream& operator<<(std::ostream& output, const DexType* dex_type) {
  output << show(dex_type);
  return output;
}
