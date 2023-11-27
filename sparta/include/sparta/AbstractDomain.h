/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <ostream>
#include <type_traits>
#include <utility>

#include <sparta/Exceptions.h>

namespace sparta {
namespace ad_impl {
template <typename Value, typename Derived>
class AbstractDomainScaffoldingStaticAssert;
} // namespace ad_impl

/*
 * This is an API for abstract domains, which are the fundamental structures in
 * Abstract Interpretation, as described in the seminal paper:
 *
 *  Patrick Cousot & Radhia Cousot. Abstract interpretation: a unified lattice
 *  model for static analysis of programs by construction or approximation of
 *  fixpoints. In Conference Record of the Fourth Annual ACM SIGPLAN-SIGACT
 *  Symposium on Principles of Programming Languages, pages 238—252, 1977.
 *
 * Abstract domains were originally defined as lattices, but this is not a
 * hard requirement. As long as the join and meet operation are sound
 * approximations of the corresponding union and intersection operations on the
 * concrete domain, one can perform computations in a sound manner. Please see
 * the following paper for a description of more general Abstract Interpretation
 * frameworks:
 *
 *  Patrick Cousot & Radhia Cousot. Abstract interpretation frameworks. Journal
 *  of Logic and Computation, 2(4):511—547, 1992.
 *
 * This API has been designed with performance in mind. This is why an element
 * of an abstract domain is mutable and all basic operations have side effects.
 * A functional interface is provided for convenience as a layer on top of these
 * operations. An abstract domain is thread safe, as all side-effecting
 * operations are guaranteed to be only invoked on thread-local objects. It is
 * the responsibility of the fixpoint operators to ensure that this assumption
 * is always verified.
 *
 * In order to avoid the use of type casts, the interface for abstract domains
 * is defined using the curiously recurring template pattern (CRTP). The final
 * class derived from an abstract domain is passed as a parameter to the base
 * domain. This guarantees that all operations carry the type of the derived
 * class. The standard usage is:
 *
 *   class MyDomain final : public AbstractDomain<MyDomain> { ... };
 *
 * All generic abstract domain combinators should follow the CRTP.
 *
 * Sample usage:
 *
 *  class MyDomain final : public AbstractDomain<MyDomain> {
 *   public:
 *    bool is_bottom() { ... }
 *    ...
 *  };
 *
 */
template <typename Derived>
class AbstractDomain {
 public:
  ~AbstractDomain() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(std::is_base_of<AbstractDomain<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractDomain");
    static_assert(std::is_final<Derived>::value, "Derived is not final");

    // Derived();
    static_assert(std::is_default_constructible<Derived>::value,
                  "Derived is not default constructible");

    // Derived(const Derived&);
    static_assert(std::is_copy_constructible<Derived>::value,
                  "Derived is not copy constructible");

    // Derived& operator=(const Derived&);
    static_assert(std::is_copy_assignable<Derived>::value,
                  "Derived is not copy assignable");

    // top() and bottom() are factory methods that respectively produce the top
    // and bottom values. We define default implementations for them in terms
    // of set_to_{top, bottom}, but implementors may wish to override those
    // implementations with more efficient versions. Here we check that any
    // such overrides bear the correct method signature.

    // static Derived bottom();
    static_assert(std::is_same<decltype(Derived::bottom()), Derived>::value,
                  "Derived::bottom() does not exist");

    // static Derived top();
    static_assert(std::is_same<decltype(Derived::top()), Derived>::value,
                  "Derived::top() does not exist");

    // bool is_bottom() const;
    static_assert(
        std::is_same<decltype(std::declval<const Derived>().is_bottom()),
                     bool>::value,
        "Derived::is_bottom() does not exist");

    // bool is_top() const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().is_top()),
                               bool>::value,
                  "Derived::is_bottom() does not exist");

    /*
     * The partial order relation.
     */
    // bool leq(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().leq(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::leq(const Derived&) does not exist");

    /*
     * a.equals(b) is semantically equivalent to a.leq(b) && b.leq(a).
     */
    // bool equals(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().equals(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::equals(const Derived&) does not exist");

    /*
     * Elements of an abstract domain are mutable and the basic operations have
     * side effects.
     */

    // void set_to_bottom();
    static_assert(
        std::is_same<decltype(std::declval<Derived>().set_to_bottom()),
                     void>::value,
        "Derived::set_to_bottom() does not exist");

    // void set_to_top();
    static_assert(std::is_same<decltype(std::declval<Derived>().set_to_top()),
                               void>::value,
                  "Derived::set_to_top() does not exist");

    /*
     * If the abstract domain is a lattice, this is the least upper bound
     * operation.
     */
    // void join_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().join_with(
                                   std::declval<const Derived>())),
                               void>::value,
                  "Derived::join_with(const Derived&) does not exist");

    /*
     * If the abstract domain has finite ascending chains, one doesn't need to
     * define a widening operator and can simply use the join instead.
     */
    // void widen_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().widen_with(
                                   std::declval<const Derived>())),
                               void>::value,
                  "Derived::widen_with(const Derived&) does not exist");

    /*
     * If the abstract domain is a lattice, this is the greatest lower bound
     * operation.
     */
    // void meet_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().meet_with(
                                   std::declval<const Derived>())),
                               void>::value,
                  "Derived::meet_with(const Derived&) does not exist");

    /*
     * If the abstract domain has finite descending chains, one doesn't need to
     * define a narrowing operator and can simply use the meet instead.
     */
    // void narrow_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().narrow_with(
                                   std::declval<const Derived>())),
                               void>::value,
                  "Derived::narrow_with(const Derived&) does not exist");
  }

  /*
   * Many C++ libraries default to using operator== to check for equality,
   * so we define it here as an alias of equals().
   */
  friend bool operator==(const Derived& self, const Derived& other) {
    return self.equals(other);
  }

  friend bool operator!=(const Derived& self, const Derived& other) {
    return !self.equals(other);
  }

  /*
   * This is a functional interface on top of the side-effecting API provided
   * for convenience.
   */

  Derived join(const Derived& other) const {
    // Here and below: the static_cast is required in order to instruct
    // the compiler to use the copy constructor of the derived class.
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.join_with(other);
    return tmp;
  }

  Derived widening(const Derived& other) const {
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.widen_with(other);
    return tmp;
  }

  Derived meet(const Derived& other) const {
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.meet_with(other);
    return tmp;
  }

  Derived narrowing(const Derived& other) const {
    Derived tmp(static_cast<const Derived&>(*this));
    tmp.narrow_with(other);
    return tmp;
  }

  static Derived top() {
    Derived tmp;
    tmp.set_to_top();
    return tmp;
  }

  static Derived bottom() {
    Derived tmp;
    tmp.set_to_bottom();
    return tmp;
  }
};

/*
 * When implementing an abstract domain, one often has to encode the Top and
 * Bottom values in a special way. This leads to a nontrivial case analysis when
 * writing the domain operations. The purpose of AbstractDomainScaffolding is to
 * implement this boilerplate logic provided a representation of regular
 * elements defined by the AbstractValue interface.
 */

enum class AbstractValueKind { Bottom, Value, Top };

} // namespace sparta

inline std::ostream& operator<<(std::ostream& o,
                                const sparta::AbstractValueKind& kind) {
  using namespace sparta;
  switch (kind) {
  case AbstractValueKind::Bottom: {
    o << "_|_";
    break;
  }
  case AbstractValueKind::Value: {
    o << "V";
    break;
  }
  case AbstractValueKind::Top: {
    o << "T";
    break;
  }
  }
  return o;
}

namespace sparta {

/*
 * This interface represents the structure of the regular elements of an
 * abstract domain (like a constant, an interval, a points-to set, etc.).
 * Performing operations on those regular values may yield Top or Bottom, which
 * is why we define an AbstractValueKind type to identify such situations.
 *
 * Sample usage:
 *
 *  class MyAbstractValue final : public AbstractValue<MyAbstractValue> {
 *   public:
 *    void clear() { m_table.clear(); }
 *    ...
 *   private:
 *    std::unordered_map<...> m_table;
 *  };
 *
 */
template <typename Derived>
class AbstractValue {
 public:
  ~AbstractValue() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(std::is_base_of<AbstractValue<Derived>, Derived>::value,
                  "Derived doesn't inherit from AbstractValue");
    static_assert(std::is_final<Derived>::value, "Derived is not final");

    // Derived();
    static_assert(std::is_default_constructible<Derived>::value,
                  "Derived is not default constructible");

    // Derived(const Derived&);
    static_assert(std::is_copy_constructible<Derived>::value,
                  "Derived is not copy constructible");

    // Derived& operator=(const Derived&);
    static_assert(std::is_copy_assignable<Derived>::value,
                  "Derived is not copy assignable");

    /*
     * When the result of an operation is Top or Bottom, we no longer need an
     * explicit representation for the abstract value. This method frees the
     * memory used to represent an abstract value (hash tables, vectors, etc.).
     */
    // void clear();
    static_assert(
        std::is_same<decltype(std::declval<Derived>().clear()), void>::value,
        "Derived::clear() does not exist");

    /*
     * Even though we factor out the logic for Top and Bottom, these elements
     * may still be represented by regular abstract values (for example [0, -1]
     * and
     * [-oo, +oo] in the domain of intervals), hence the need for such a method.
     */
    // AbstractValueKind kind() const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().kind()),
                               AbstractValueKind>::value,
                  "Derived::kind() does not exist");

    /*
     * The partial order relation.
     */
    // bool leq(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().leq(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::leq(const Derived&) does not exist");

    /*
     * a.equals(b) is semantically equivalent to a.leq(b) && b.leq(a).
     */
    // bool equals(const Derived& other) const;
    static_assert(std::is_same<decltype(std::declval<const Derived>().equals(
                                   std::declval<const Derived>())),
                               bool>::value,
                  "Derived::equals(const Derived&) does not exist");

    /*
     * These are the regular abstract domain operations that perform side
     * effects. They return a AbstractValueKind value to identify situations
     * where the result of the operation is either Top or Bottom.
     */

    // AbstractValueKind join_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().join_with(
                                   std::declval<const Derived>())),
                               AbstractValueKind>::value,
                  "Derived::join_with(const Derived&) does not exist");

    // AbstractValueKind widen_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().widen_with(
                                   std::declval<const Derived>())),
                               AbstractValueKind>::value,
                  "Derived::widen_with(const Derived&) does not exist");

    // AbstractValueKind meet_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().meet_with(
                                   std::declval<const Derived>())),
                               AbstractValueKind>::value,
                  "Derived::meet_with(const Derived&) does not exist");

    // AbstractValueKind narrow_with(const Derived& other);
    static_assert(std::is_same<decltype(std::declval<Derived>().narrow_with(
                                   std::declval<const Derived>())),
                               AbstractValueKind>::value,
                  "Derived::narrow_with(const Derived&) does not exist");
  }
};

/*
 * This exception flags the use of an abstract value with invalid kind in the
 * given context.
 */
class invalid_abstract_value
    : public virtual abstract_interpretation_exception {};

using expected_kind =
    boost::error_info<struct tag_expected_kind, AbstractValueKind>;

using actual_kind =
    boost::error_info<struct tag_actual_kind, AbstractValueKind>;

/*
 * This abstract domain combinator takes an abstract value specification and
 * constructs a full-fledged abstract domain, handling all the logic for Top and
 * Bottom. It takes a poset and adds the two extremal elements Top and Bottom.
 * If the poset contains a Top and/or Bottom element, then those should be
 * coalesced with the extremal elements added by AbstractDomainScaffolding. It
 * also explains why the lattice operations return an AbstractValueKind: the
 * scaffolding must be able to identify when the result of one of these
 * operations is an extremal element that must be coalesced. While the helper
 * method `normalize()` can be used to automatically coalesce the Top elements,
 * Bottom should be handled separately in each operation of the abstract domain.
 *
 * Sample usage:
 *
 * class MyAbstractValue final : public AbstractValue<MyAbstractValue> {
 *  ...
 * };
 *
 * class MyAbstractDomain final
 *     : public AbstractDomainScaffolding<MyAbstractValue, MyAbstractDomain> {
 *  public:
 *   MyAbstractDomain(...) { ... }
 *
 *   // All basic operations defined in AbstractDomain are provided by
 *   // AbstractDomainScaffolding. We only need to define the operations
 *   // that are specific to MyAbstractDomain.
 *
 *   void my_operation(...) { ... }
 *
 *   void my_other_operation(...) { ... }
 * };
 *
 */
template <typename Value, typename Derived>
class AbstractDomainScaffolding
    : public AbstractDomain<Derived>,
      private ad_impl::AbstractDomainScaffoldingStaticAssert<Value, Derived> {
 public:
  /*
   * The choice of lattice element returned by the default constructor is
   * completely arbitrary. In pratice though, the abstract value used to
   * initiate a fixpoint iteration is most often constructed in this way.
   */
  AbstractDomainScaffolding() { m_kind = m_value.kind(); }

  /*
   * A convenience constructor for creating Bottom and Top.
   */
  explicit AbstractDomainScaffolding(AbstractValueKind kind) : m_kind(kind) {
    SPARTA_RUNTIME_CHECK(kind != AbstractValueKind::Value,
                         invalid_abstract_value() << actual_kind(kind));
  }

  AbstractValueKind kind() const { return m_kind; }

  bool is_bottom() const { return m_kind == AbstractValueKind::Bottom; }

  bool is_top() const { return m_kind == AbstractValueKind::Top; }

  bool is_value() const { return m_kind == AbstractValueKind::Value; }

  void set_to_bottom() {
    m_kind = AbstractValueKind::Bottom;
    m_value.clear();
  }

  void set_to_top() {
    m_kind = AbstractValueKind::Top;
    m_value.clear();
  }

  bool leq(const Derived& other) const {
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
    SPARTA_RUNTIME_CHECK(m_kind == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(m_kind));
    SPARTA_RUNTIME_CHECK(other.m_kind == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(other.m_kind));
    return m_value.leq(other.m_value);
  }

  bool equals(const Derived& other) const {
    if (is_bottom()) {
      return other.is_bottom();
    }
    if (is_top()) {
      return other.is_top();
    }
    SPARTA_RUNTIME_CHECK(m_kind == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(m_kind));
    if (other.m_kind != AbstractValueKind::Value) {
      return false;
    }
    return m_value.equals(other.m_value);
  }

  void join_with(const Derived& other) {
    auto value_join = [this, &other]() {
      m_kind = m_value.join_with(other.m_value);
    };
    join_like_operation_with(other, value_join);
  }

  void widen_with(const Derived& other) {
    auto value_widening = [this, &other]() {
      m_kind = m_value.widen_with(other.m_value);
    };
    join_like_operation_with(other, value_widening);
  }

  void meet_with(const Derived& other) {
    auto value_meet = [this, &other]() {
      m_kind = m_value.meet_with(other.m_value);
    };
    meet_like_operation_with(other, value_meet);
  }

  void narrow_with(const Derived& other) {
    auto value_narrowing = [this, &other]() {
      m_kind = m_value.narrow_with(other.m_value);
    };
    meet_like_operation_with(other, value_narrowing);
  }

 protected:
  // These methods allow the derived class to manipulate the abstract value.
  Value* get_value() { return &m_value; }

  const Value* get_value() const { return &m_value; }

  void set_to_value(Value value) {
    m_kind = value.kind();
    m_value = std::move(value);
  }

  // In some implementations, the data structure chosen to represent an abstract
  // value can also denote Top (e.g., when using a map to represent an abstract
  // environment, the empty map commonly denotes Top). This method is used to
  // keep the internal representation and the m_kind flag in sync after
  // performing an operation. It should never be used to infer Bottom from the
  // internal representation of an abstract value. Bottom should always be
  // treated as a special case in every operation of the abstract domain.
  void normalize() {
    if (m_kind == AbstractValueKind::Bottom) {
      return;
    }
    // Consider an abstract environment represented as a map. After removing
    // some bindings from the environment, the internal map might become empty
    // and the m_kind flag must be set to AbstractValueKind::Top. Conversely,
    // after adding a binding to the abstract environment Top, the internal map
    // is no longer empty and m_kind must be set to AbstractValueKind::Value.
    // This step synchronizes both representations.
    m_kind = m_value.kind();
    if (m_kind == AbstractValueKind::Top) {
      // We can discard any leftover data from the internal representation.
      m_value.clear();
    }
  }

 private:
  template <typename Operation> // void()
  void join_like_operation_with(const Derived& other, Operation&& operation) {
    if (is_top() || other.is_bottom()) {
      return;
    }
    if (other.is_top()) {
      set_to_top();
      return;
    }
    if (is_bottom()) {
      m_kind = other.m_kind;
      m_value = other.m_value;
      return;
    }
    operation();
  }

  template <typename Operation> // void()
  void meet_like_operation_with(const Derived& other, Operation&& operation) {
    if (is_bottom() || other.is_top()) {
      return;
    }
    if (other.is_bottom()) {
      set_to_bottom();
      return;
    }
    if (is_top()) {
      m_kind = other.m_kind;
      m_value = other.m_value;
      return;
    }
    operation();
  }

  AbstractValueKind m_kind;
  Value m_value;
};

// Implement copy on write for Value. The FixpointIterator makes lots of copies
// of Domain objects. This class delays copying Value until we actually need to
// (before a write). m_value is read-only until the first write operation.
//
// This AbstractValue is recommended whenever copying the underlying abstract
// value incurs a significant cost
template <typename Value>
class CopyOnWriteAbstractValue final
    : AbstractValue<CopyOnWriteAbstractValue<Value>> {
  using This = CopyOnWriteAbstractValue<Value>;

 public:
  void clear() { get().clear(); }

  AbstractValueKind kind() const { return get().kind(); }

  bool leq(const This& other) const { return get().leq(other.get()); }

  bool equals(const This& other) const { return get().equals(other.get()); }

  AbstractValueKind join_with(const This& other) {
    return get().join_with(other.get());
  }

  AbstractValueKind widen_with(const This& other) {
    return get().widen_with(other.get());
  }

  AbstractValueKind meet_with(const This& other) {
    return get().meet_with(other.get());
  }

  AbstractValueKind narrow_with(const This& other) {
    return get().narrow_with(other.get());
  }

  // m_value should _only_ be accessed via `get()`
  Value& get() {
    upgrade_to_writer();
    return *m_value;
  }
  const Value& get() const { return *m_value; }

 private:
  // WARNING: This Copy on write implementation is likely to fail with multiple
  // concurrent accesses
  void upgrade_to_writer() {
    if (m_value.use_count() > 1) {
      // need to make a copy
      m_value = std::make_shared<Value>(*m_value);
    }
  }

  // m_value should only be accessed via `get()`
  std::shared_ptr<Value> m_value{std::make_shared<Value>()};
};

/*
 * This reverses the top / bottom elements and meet / join operations of the
 * domain. It also switches widen / narrow, which is *only* valid for finite
 * abstract domains, where the widening / narrowing are identical to the join /
 * meet operations.
 */
template <typename Domain, typename Derived>
class AbstractDomainReverseAdaptor : public AbstractDomain<Derived> {
 public:
  using BaseDomain = Domain;

  AbstractDomainReverseAdaptor() = default;

  template <typename... Args>
  explicit AbstractDomainReverseAdaptor(Args&&... args)
      : m_domain(std::forward<Args>(args)...) {}

  bool is_bottom() const { return m_domain.is_top(); }

  bool is_top() const { return m_domain.is_bottom(); }

  bool leq(const Derived& other) const {
    return m_domain.equals(other.m_domain) || !m_domain.leq(other.m_domain);
  }

  bool equals(const Derived& other) const {
    return m_domain.equals(other.m_domain);
  }

  void set_to_bottom() { m_domain.set_to_top(); }

  void set_to_top() { m_domain.set_to_bottom(); }

  void join_with(const Derived& other) { m_domain.meet_with(other.m_domain); }

  void widen_with(const Derived& other) {
    m_domain.narrow_with(other.m_domain);
  }

  void meet_with(const Derived& other) { m_domain.join_with(other.m_domain); }

  void narrow_with(const Derived& other) {
    m_domain.widen_with(other.m_domain);
  }

  static Derived bottom() { return Derived(Domain::top()); }

  static Derived top() { return Derived(Domain::bottom()); }

  Domain& unwrap() { return m_domain; }
  const Domain& unwrap() const { return m_domain; }

 private:
  Domain m_domain;
};

} // namespace sparta

template <typename Domain, typename Derived>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::AbstractDomainReverseAdaptor<Domain, Derived>& d) {
  o << d.unwrap();
  return o;
}

namespace sparta::ad_impl {

template <typename Value, typename Derived>
class AbstractDomainScaffoldingStaticAssert {
 protected:
  ~AbstractDomainScaffoldingStaticAssert() {
    static_assert(std::is_base_of_v<AbstractValue<Value>, Value>,
                  "Value doesn't inherit from AbstractValue");
    static_assert(
        std::is_base_of_v<AbstractDomainScaffolding<Value, Derived>, Derived>,
        "Derived doesn't inherit from AbstractDomainScaffolding");
    static_assert(std::is_final<Derived>::value, "Derived is not final");
  }
};

} // namespace sparta::ad_impl
