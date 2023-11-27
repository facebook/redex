/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <ostream>
#include <type_traits>

#include <sparta/AbstractDomain.h>
#include <sparta/Exceptions.h>

namespace sparta {

/*
 * Augments an underlying domain -- D -- with a new least element.  In
 * documentation and output formats, the underlying domain's existing least
 * element will be referred to by the symbol * and the new least element as _|_.
 *
 * See Page 39 of Moeller and Schwartzbach [0] for potential uses of this
 * combinator in abstract interpretation.
 *
 *  [0] Anders Moeller and Michael I. Schwatzbach. Static Program Analysis.
 *        (December 2019). Retrieved April 23, 2020 from
 *        https://cs.au.dk/~amoeller/spa/spa.pdf
 */
template <typename D>
class LiftedDomain final : public AbstractDomain<LiftedDomain<D>> {
  static_assert(std::is_convertible<D*, AbstractDomain<D>*>::value,
                "LiftedDomain must wrap another domain.");

 public:
  static LiftedDomain bottom() { return LiftedDomain{nullptr}; }

  static LiftedDomain lifted(D underlying) {
    return LiftedDomain{std::make_unique<D>(std::move(underlying))};
  }

  static LiftedDomain top() {
    return LiftedDomain{std::make_unique<D>(D::top())};
  }

  /* Default constructor produces the default value in underlying domain. */
  LiftedDomain() : LiftedDomain(std::make_unique<D>()) {}

  LiftedDomain(LiftedDomain&&) = default;
  LiftedDomain& operator=(LiftedDomain&&) = default;

  LiftedDomain(const LiftedDomain& that) {
    if (that.m_underlying) {
      m_underlying = std::make_unique<D>(*that.m_underlying);
    }
  }

  LiftedDomain& operator=(const LiftedDomain& that) {
    m_underlying = that.m_underlying ? std::make_unique<D>(*that.m_underlying)
                                     : std::unique_ptr<D>();
    return *this;
  }

  bool is_bottom() const { return !m_underlying; }

  bool is_top() const { return m_underlying && m_underlying->is_top(); }

  bool is_lifted() const { return !is_bottom(); }

  D& lowered() {
    SPARTA_RUNTIME_CHECK(is_lifted(), undefined_operation());
    return *m_underlying;
  }

  const D& lowered() const {
    SPARTA_RUNTIME_CHECK(is_lifted(), undefined_operation());
    return *m_underlying;
  }

  bool leq(const LiftedDomain& that) const {
    if (is_bottom()) {
      return true;
    } else if (that.is_bottom()) {
      return false;
    } else {
      return m_underlying->leq(*that.m_underlying);
    }
  }

  bool equals(const LiftedDomain& that) const {
    return (is_bottom() && that.is_bottom()) ||
           m_underlying->equals(*that.m_underlying);
  }

  void set_to_bottom() { m_underlying = nullptr; }

  void set_to_top() { m_underlying = std::make_unique<D>(D::top()); }

  /*
   *  _|_ \/  x  = x
   *   x  \/ _|_ = x
   *   x  \/  y  = x  \/' y
   *
   * Where \/' is the join on the underlying domain.
   */
  void join_with(const LiftedDomain& that) {
    if (is_bottom()) {
      *this = that;
    } else if (!that.is_bottom()) {
      m_underlying->join_with(*that.m_underlying);
    }
  }

  /*
   *  _|_ W  x  = x
   *   x  W _|_ = x
   *   x  W  y  = x  W' y
   *
   * Where W' is the widening on the underlying domain.
   */
  void widen_with(const LiftedDomain& that) {
    if (is_bottom()) {
      *this = that;
    } else if (!that.is_bottom()) {
      m_underlying->widen_with(*that.m_underlying);
    }
  }

  /*
   *  _|_ /\  _  = _|_
   *   _  /\ _|_ = _|_
   *   x  /\  y  =  x  /\' y
   *
   * Where /\' is the meet on the underlying domain.
   */
  void meet_with(const LiftedDomain& that) {
    if (is_bottom()) {
      return;
    } else if (that.is_bottom()) {
      set_to_bottom();
    } else {
      m_underlying->meet_with(*that.m_underlying);
    }
  }

  /*
   *  _|_ N  x  = _|_
   *   x  N _|_ = _|_
   *   x  N  y  = x  N' y
   *
   * Where N' is the narrowing on the underlying domain.
   */
  void narrow_with(const LiftedDomain& that) {
    if (is_bottom()) {
      return;
    } else if (that.is_bottom()) {
      set_to_bottom();
    } else {
      m_underlying->narrow_with(*that.m_underlying);
    }
  }

 private:
  // Bottom for the lifted domain is represented by a null underlying pointer.
  std::unique_ptr<D> m_underlying = nullptr;

  explicit LiftedDomain(std::unique_ptr<D> underlying)
      : m_underlying(std::move(underlying)) {}
};

} // namespace sparta

template <typename D>
inline std::ostream& operator<<(std::ostream& o,
                                const sparta::LiftedDomain<D>& i) {
  if (i.is_bottom()) {
    return o << "_|_";
  }

  if (i.is_top()) {
    return o << "T";
  }

  const auto& under = i.lowered();
  if (under.is_bottom()) {
    return o << "*";
  }

  return o << under;
}
