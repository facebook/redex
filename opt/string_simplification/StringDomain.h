/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <algorithm>
#include <boost/optional.hpp>
#include <iostream>
#include <tuple>

#include "AbstractDomain.h"
#include "HashedSetAbstractDomain.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "ReducedProductAbstractDomain.h"
#include "SimpleValueAbstractDomain.h"

using string_register_t = uint16_t;
using pointer_reference_t = uint32_t;

/**
 * This class represents constant strings living in the following lattice:
 *                           T ( Any string e.g some unknown variable )
 *
 *                  /        |         \
 *
 *    [Concrete string values, with pointers to registers (static / non static)]
 *
 *                  \        |        /
 *
 *                          _|_ ( Invalid configuration )
 *
 * e.g. String s = x + "const"; where x is a different string and "const"
 * is some constant that is appended to the right.  x is allowed to be
 * variable (unknown).  We can reconstruct s by taking the base register holding
 * x and then appending "const" to it.
 *
 */
class StringyValue final : public AbstractValue<StringyValue> {
 public:
  friend class StringyDomain;

  void clear() override{};

  AbstractValueKind kind() const override { return AbstractValueKind::Value; }

  bool equals(const StringyValue& other) const override {
    return m_suffix == other.m_suffix && m_base_reg == other.m_base_reg;
  }

  bool leq(const StringyValue& other) const override { return equals(other); }

  AbstractValueKind join_with(const StringyValue& other) override {
    if (!equals(other)) {
      return AbstractValueKind::Top;
    } else {
      return AbstractValueKind::Value;
    }
  }

  AbstractValueKind meet_with(const StringyValue& other) override {
    if (!equals(other)) {
      return AbstractValueKind::Bottom;
    } else {
      return AbstractValueKind::Value;
    }
  }

  AbstractValueKind widen_with(const StringyValue& other) override {
    return join_with(other);
  }

  AbstractValueKind narrow_with(const StringyValue& other) override {
    return meet_with(other);
  }

  bool is_static_string() const { return m_static_string; }

  std::string suffix() const { return m_suffix; }

  bool has_base() const { return (m_base_reg) ? true : false; }

  string_register_t base() const { return *m_base_reg; }

  StringyValue(std::string suffix = "",
               boost::optional<string_register_t> base_reg = boost::none,
               bool static_string = false)
      : m_suffix(suffix),
        m_base_reg(base_reg),
        m_static_string(static_string) {}

 private:
  std::string m_suffix;
  boost::optional<string_register_t> m_base_reg;
  bool m_static_string;
};

inline std::ostream& operator<<(std::ostream& o, const StringyValue& sv) {
  if (sv.is_static_string()) {
    o << "const[" << sv.suffix() << "]";
  } else {
    o << "builder[";
    if (sv.has_base()) {
      o << "v" << sv.base() << "+";
    }
    o << '"' << sv.suffix() << "\"]";
  }
  return o;
}

class StringyDomain final
    : public AbstractDomainScaffolding<StringyValue, StringyDomain> {
 public:
  friend std::ostream& operator<<(std::ostream& o, const StringyDomain& sd);

  static StringyDomain bottom() {
    return StringyDomain(AbstractValueKind::Bottom);
  }

  static StringyDomain top() { return StringyDomain(AbstractValueKind::Top); }

  static StringyDomain value(
      std::string suffix,
      boost::optional<string_register_t> base = boost::none,
      bool is_static_string = false) {
    StringyDomain result;
    result.set_to_value(StringyValue(suffix, base, is_static_string));
    return result;
  }

  static StringyDomain append(StringyDomain original,
                              boost::optional<string_register_t> reg,
                              std::string suffix) {
    if (original.is_top()) {
      return StringyDomain::value(suffix, reg);
    } else if (original.is_bottom()) {
      return StringyDomain::bottom();
    } else {
      always_assert(!original.value().is_static_string());
      StringyDomain result;
      result.set_to_value(StringyValue(
          std::string(original.value().suffix()) + std::string(suffix),
          original.value().m_base_reg,
          original.value().m_static_string));
      return result;
    }
  }

  StringyDomain(AbstractValueKind kind = AbstractValueKind::Top)
      : AbstractDomainScaffolding<StringyValue, StringyDomain>(kind) {}

  StringyValue value() const { return *get_value(); }
};

inline std::ostream& operator<<(std::ostream& o, const StringyDomain& sd) {
  if (sd.is_bottom()) {
    o << "_|_";
  } else if (sd.is_top()) {
    o << "T";
  } else {
    o << sd.value();
  }
  return o;
}

using PointerDomain = SimpleValueAbstractDomain<pointer_reference_t>;

using PointerReferenceEnvironment =
    PatriciaTreeMapAbstractEnvironment<string_register_t, PointerDomain>;

using StringConstantEnvironment =
    PatriciaTreeMapAbstractEnvironment<pointer_reference_t, StringyDomain>;

// We need a layer of indirection to be able to solve the pointer analysis
// during the string concatenation because multiple registers can point to the
// same StringBuilder.
class StringProdEnvironment final
    : public ReducedProductAbstractDomain<StringProdEnvironment,
                                          PointerReferenceEnvironment,
                                          StringConstantEnvironment> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      std::tuple<PointerReferenceEnvironment,
                 StringConstantEnvironment>& /* product */) {}

  static StringProdEnvironment top() {
    StringProdEnvironment p;
    p.set_to_top();
    return p;
  }

  static StringProdEnvironment bottom() {
    StringProdEnvironment p;
    p.set_to_bottom();
    return p;
  }

  StringyDomain eval(string_register_t reg) const {
    auto ptr = get<0>().get(reg);
    if (ptr.is_value()) {
      return get<1>().get(ptr.value());
    }
    return StringyDomain::top();
  }

  void put(string_register_t reg, StringyDomain val) {
    auto ptr = get<0>().get(reg);
    if (!ptr.is_value()) {
      create(reg);
    }
    auto id = get<0>().get(reg).value();
    apply<1>([=](auto env) { env->set(id, val); }, true);
  }

  void create(string_register_t reg) {
    pointer_reference_t id = new_pointer();
    apply<0>([=](auto env) { env->set(reg, PointerDomain::value(id)); }, true);
  }

  void move(string_register_t dest, string_register_t src) {
    apply<0>([=](auto env) { env->set(dest, env->get(src)); }, true);
  }

  void clear(string_register_t reg) {
    apply<0>([=](auto env) { env->set(reg, PointerDomain::top()); }, true);
  }

  bool is_tracked(string_register_t reg) {
    return get<0>().get(reg).is_value();
  }

  PointerDomain get_id(string_register_t reg) const {
    return get<0>().get(reg);
  }

 private:
  // Create a new pointer to the object heap by finding the highest
  // object id. (We need to scan both domains because some values can be top)
  pointer_reference_t new_pointer() {
    pointer_reference_t max = 0;
    if (get<0>().is_value()) {
      for (const auto& var : get<0>().bindings()) {
        auto ptr = var.second;
        max = std::max(max, ptr.value());
      }
    }
    if (get<1>().is_value()) {
      for (const auto& var : get<1>().bindings()) {
        auto ptr = var.first;
        max = std::max(max, ptr);
      }
    }
    return max + 1;
  }
};
