/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <bitset>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AbstractDomain.h"
#include "Exceptions.h"

namespace sparta {

namespace fad_impl {

template <typename Element, size_t kCardinality>
class BitVectorSemiLattice;

} // namespace fad_impl

/*
 * This is the general interface for arbitrary encodings of a lattice. 'Element'
 * is the type of the symbolic names for the lattice elements and 'Encoding' is
 * the type of the actual encoding.
 */
template <typename Element, typename Encoding>
class LatticeEncoding {
 public:
  virtual ~LatticeEncoding() {}

  virtual Encoding encode(const Element& element) const = 0;

  virtual Element decode(const Encoding& encoding) const = 0;

  virtual bool is_bottom(const Encoding& element) const = 0;

  virtual bool is_top(const Encoding& element) const = 0;

  virtual bool equals(const Encoding& x, const Encoding& y) const = 0;

  virtual bool leq(const Encoding& x, const Encoding& y) const = 0;

  virtual Encoding join(const Encoding& x, const Encoding& y) const = 0;

  virtual Encoding meet(const Encoding& x, const Encoding& y) const = 0;

  virtual Encoding bottom() const = 0;

  virtual Encoding top() const = 0;
};

/*
 * Example usage:
 *
 *   Encoding the following lattice using bit vectors:
 *
 *              TOP
 *             /   \
 *            A     B
 *             \   /
 *             BOTTOM
 *
 *   enum Elements {BOTTOM, A, B, TOP};
 *   using Lattice = BitVectorLattice<Elements, 4>;
 *   Lattice lattice({BOTTOM, A, B, TOP},
 *                   {{BOTTOM, A}, {BOTTOM, B}, {A, TOP}, {B, TOP}});
 *   using Domain =
 *       FiniteAbstractDomain<Elements, Lattice, Lattice::Encoding, &lattice>;
 *   ...
 *   Domain a(A), b(B);
 *   Domain x = a.join(b);
 *   ...
 *
 * Note: since 'lattice' is a template argument, this object must be statically
 * defined, for example as a global variable. The lattice is instantiated just
 * once at startup time.
 *
 */
template <typename Element,
          typename Lattice,
          typename Encoding,
          Lattice* lattice>
class FiniteAbstractDomain final
    : public AbstractDomain<
          FiniteAbstractDomain<Element, Lattice, Encoding, lattice>> {
 public:
  ~FiniteAbstractDomain() {
    // The destructor is the only method that is guaranteed to be created when a
    // class template is instantiated. This is a good place to perform all the
    // sanity checks on the template parameters.
    static_assert(
        std::is_base_of<LatticeEncoding<Element, Encoding>, Lattice>::value,
        "Lattice doesn't derive from LatticeEncoding");
  }

  /*
   * A default constructor is required in the AbstractDomain specification.
   */
  FiniteAbstractDomain() : m_encoding(lattice->top()) {}

  explicit FiniteAbstractDomain(const Element& element)
      : m_encoding(lattice->encode(element)) {}

  Element element() const { return lattice->decode(m_encoding); }

  bool is_bottom() const override { return lattice->is_bottom(m_encoding); }

  bool is_top() const override { return lattice->is_top(m_encoding); }

  bool leq(const FiniteAbstractDomain& other) const override {
    return lattice->leq(m_encoding, other.m_encoding);
  }

  bool equals(const FiniteAbstractDomain& other) const override {
    return lattice->equals(m_encoding, other.m_encoding);
  }

  void set_to_bottom() override { m_encoding = lattice->bottom(); }

  void set_to_top() override { m_encoding = lattice->top(); }

  void join_with(const FiniteAbstractDomain& other) override {
    m_encoding = lattice->join(m_encoding, other.m_encoding);
  }

  void widen_with(const FiniteAbstractDomain& other) override {
    join_with(other);
  }

  void meet_with(const FiniteAbstractDomain& other) override {
    m_encoding = lattice->meet(m_encoding, other.m_encoding);
  }

  void narrow_with(const FiniteAbstractDomain& other) override {
    meet_with(other);
  }

  static FiniteAbstractDomain bottom() {
    return FiniteAbstractDomain(lattice->bottom());
  }

  static FiniteAbstractDomain top() {
    return FiniteAbstractDomain(lattice->top());
  }

 private:
  FiniteAbstractDomain(const Encoding& encoding) : m_encoding(encoding) {}

  Encoding m_encoding;
};

} // namespace sparta

template <typename Element,
          typename Lattice,
          typename Encoding,
          Lattice* lattice>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::
        FiniteAbstractDomain<Element, Lattice, Encoding, lattice>& x) {
  o << x.element();
  return o;
}

namespace sparta {

/*
 * A lattice with elements encoded as bit vectors.
 *
 * This maintains two semi-lattices internally; always use the opposite
 * semi-lattice representation and calculate corresponding lower semi-lattice
 * encoding when needed.
 *
 * The last two template parameters are unused and will be eventually removed.
 */
template <typename Element,
          size_t kCardinality,
          typename = void,
          typename = void>
class BitVectorLattice final
    : public fad_impl::BitVectorSemiLattice<Element,
                                            kCardinality>::LatticeEncoding {
  using SemiLattice = fad_impl::BitVectorSemiLattice<Element, kCardinality>;

 public:
  using Encoding = typename SemiLattice::Encoding;

  /*
   * In a standard fixpoint computation the Join is by far the dominant
   * operation. Hence, we favor the opposite semi-lattice encoding whenever we
   * construct a domain element.
   *
   * However, we give the impression of operating in the given (lower) lattice
   * so everything below is opposite: top is bottom, join is meet, leq is geq,
   * etc.
   */

  BitVectorLattice() = delete;

  BitVectorLattice(
      std::initializer_list<Element> elements,
      std::initializer_list<std::pair<Element, Element>> hasse_diagram)
      : m_lower_semi_lattice(
            elements, hasse_diagram, /* construct_opposite_lattice */ false),
        m_opposite_semi_lattice(
            elements, hasse_diagram, /* construct_opposite_lattice */ true) {}

  Encoding encode(const Element& element) const override {
    return m_opposite_semi_lattice.encode(element);
  }

  Element decode(const Encoding& encoding) const override {
    return m_opposite_semi_lattice.decode(encoding);
  }

  bool is_bottom(const Encoding& x) const override {
    return m_opposite_semi_lattice.is_top(x);
  }

  bool is_top(const Encoding& x) const override {
    return m_opposite_semi_lattice.is_bottom(x);
  }

  bool equals(const Encoding& x, const Encoding& y) const override {
    return m_opposite_semi_lattice.equals(x, y);
  }

  bool leq(const Encoding& x, const Encoding& y) const override {
    return m_opposite_semi_lattice.geq(x, y);
  }

  Encoding join(const Encoding& x, const Encoding& y) const override {
    return m_opposite_semi_lattice.meet(x, y);
  }

  Encoding meet(const Encoding& x, const Encoding& y) const override {
    // In order to perform the Meet, we need to calculate corresponding lower
    // semi-lattice encoding, and switch back to opposite semi-lattice encoding
    // before returning.
    auto x_lower = get_lower_encoding(x);
    auto y_lower = get_lower_encoding(y);
    Encoding lower_encoding = m_lower_semi_lattice.meet(x_lower, y_lower);
    return get_opposite_encoding(lower_encoding);
  }

  Encoding bottom() const override { return m_opposite_semi_lattice.top(); }

  Encoding top() const override { return m_opposite_semi_lattice.bottom(); }

 private:
  Element decode_lower(const Encoding& encoding) const {
    return m_lower_semi_lattice.decode(encoding);
  }

  Encoding get_lower_encoding(const Encoding& x) const {
    const Element& element = decode(x);
    return m_lower_semi_lattice.encode(element);
  }

  Encoding get_opposite_encoding(const Encoding& x) const {
    const Element& element = decode_lower(x);
    return m_opposite_semi_lattice.encode(element);
  }

  SemiLattice m_lower_semi_lattice;
  SemiLattice m_opposite_semi_lattice;
};

namespace fad_impl {

template <typename Element, typename ElementAsInt = Element>
bool is_zero_based_integer_range(
    const std::initializer_list<Element>& elements) {
  if constexpr (std::is_integral_v<ElementAsInt>) {
    std::vector<bool> seen(elements.size());
    for (const auto element : elements) {
      const auto element_int = static_cast<ElementAsInt>(element);
      if (element_int >= 0) {
        const auto element_index = static_cast<size_t>(element_int);
        if (element_index < seen.size() && !seen[element_index]) {
          seen[element_index] = true;
          continue;
        }
      }

      // Negative, beyond cardinality, or duplicated.
      return false;
    }

    // We saw K unique elements in the range [0, K).
    return true;
  } else if constexpr (std::is_enum_v<ElementAsInt>) {
    using Underlying = std::underlying_type_t<Element>;
    return is_zero_based_integer_range<Element, Underlying>(elements);
  } else {
    return false;
  }
}

/*
 * Our encoding of lattices is based on the following paper that proposes an
 * efficient representation based on bit vectors:
 *
 *   H. Aït-Kaci, R. Boyer, P. Lincoln, R. Nasr. Efficient implementation of
 *   lattice operations. In ACM Transactions on Programming Languages and
 *   Systems (TOPLAS), Volume 11, Issue 1, Jan. 1989, pages 115-146.
 *
 * The approach described in the paper only works with the Meet operation. The
 * idea is to represent the Hasse diagram of a lattice using a Boolean matrix,
 * as shown below:
 *
 *         d                          a  b  c  d
 *        / \                      a  0  0  0  0
 *       b   c                     b  1  0  0  0
 *        \ /                      c  1  0  0  0
 *         a                       d  0  1  1  0
 *
 * This matrix represents the "immediately greater than" relation in the
 * lattice. The technique consists of computing the reflexive and transitive
 * closure of that relation. Then, an element can be encoded by its
 * corresponding row (i.e., a bit vector) in the resulting matrix. Computing the
 * Meet simply amounts to performing the bitwise And operation on the bit
 * vector representation. For the example above that gives:
 *
 * Reflexive-transitive closure:
 *
 *               a  b  c  d                b Meet c = 1100 & 1010
 *            a  1  0  0  0                         = 1000
 *            b  1  1  0  0                         = a
 *            c  1  0  1  0
 *            d  1  1  1  1
 *
 * In order to compute the Join, we apply the same technique to the opposite
 * lattice, i.e., the lattice in which the order relation has been reversed and
 * the Top and Bottom elements have been swapped. The opposite lattice and the
 * corresponding Boolean matrix are constructed as follows:
 *
 *         a                          a  b  c  d
 *        / \                      a  0  1  1  0
 *       b   c                     b  0  0  0  1
 *        \ /                      c  0  0  0  1
 *         d                       d  0  0  0  0
 *
 * It can be easily seen that the Meet in the opposite lattice is exactly the
 * Join in the original lattice.
 *
 * Reflexive-transitive closure:
 *
 *               a  b  c  d                b Meet c = 0101 & 0011
 *            a  1  1  1  1                         = 0001
 *            b  0  1  0  1                         = d
 *            c  0  0  1  1                         = b Join c in the original
 *            d  0  0  0  1                           lattice
 *
 * The constructor parameter 'construct_opposite_lattice' specifies the lattice
 * to consider for the encoding.
 *
 * Note that constructing this representation has cubic time complexity in the
 * number of elements of the lattice. Since the construction is done only once
 * at startup time and finite lattices built this way are usually small, this
 * should not be a problem in practice.
 *
 */
template <typename Element, size_t kCardinality>
class BitVectorSemiLattice final {
 public:
  static_assert(kCardinality >= 2, "Lattice must have at least 2 elements.");

  using Encoding = std::bitset<kCardinality>;

  // The lattice encoding we're used to implement.
  using LatticeEncoding = ::sparta::LatticeEncoding<Element, Encoding>;

  BitVectorSemiLattice() = delete;

  /*
   * In order to construct the bit vector representation, the user provides the
   * complete set of elements in the lattice (including the Top and Bottom
   * elements) as well as the Hasse diagram of the partial order relation.
   */
  BitVectorSemiLattice(
      std::initializer_list<Element> elements,
      std::initializer_list<std::pair<Element, Element>> hasse_diagram,
      bool construct_opposite_lattice) {
    // For efficiency we will require elements be integer-like and in a
    // continuous range [0, K). We could relax this in various ways and have
    // fallback cases, but every current usage satisfies this restriction.
    RUNTIME_CHECK(elements.size() == kCardinality,
                  invalid_argument()
                      << argument_name("elements")
                      << operation_name("BitVectorSemiLattice()"));
    RUNTIME_CHECK(is_zero_based_integer_range(elements),
                  invalid_argument()
                      << argument_name("elements")
                      << operation_name("BitVectorSemiLattice()"));

    // We populate the Boolean matrix by traversing the Hasse diagram of the
    // partial order.
    Encoding matrix[kCardinality];
    for (auto pair : hasse_diagram) {
      // The Hasse diagram provided by the user describes the partial order in
      // the original lattice. We need to normalize the representation when the
      // opposite lattice is considered.
      if (construct_opposite_lattice) {
        std::swap(pair.first, pair.second);
      }

      // If y is immediately greater than x in the partial order considered,
      // then matrix[y][x] = 1.
      const auto x_idx = element_to_index(pair.first);
      const auto y_idx = element_to_index(pair.second);
      matrix[y_idx][x_idx] = true;
    }

    // We first compute the reflexive closure of the "immediately greater than"
    // relation in the lattice considered.
    for (size_t i = 0; i < kCardinality; ++i) {
      matrix[i][i] = true;
    }

    // Then we compute the transitive closure of the "immediately greater than"
    // relation in the lattice considered, using Warshall's algorithm.
    for (size_t k = 0; k < kCardinality; ++k) {
      for (size_t i = 0; i < kCardinality; ++i) {
        for (size_t j = 0; j < kCardinality; ++j) {
          matrix[i][j] = matrix[i][j] || (matrix[i][k] && matrix[k][j]);
        }
      }
    }

    // The last step is to assign a bit vector representation to each element in
    // the lattice considered, i.e. the corresponding row in the Boolean matrix.
    // We also maintain a reverse table for decoding purposes.
    for (size_t i = 0; i < kCardinality; ++i) {
      Element element = index_to_element(i);
      Encoding encoding = matrix[i];
      m_element_to_encoding[element] = encoding;
      m_encoding_to_element[encoding] = element;
      // We identify the Bottom and Top elements on the fly.
      if (is_bottom(encoding)) {
        m_bottom = encoding;
      }
      if (is_top(encoding)) {
        m_top = encoding;
      }
    }

    // Make sure that we obtain a semi-lattice.
    sanity_check();
  }

  inline static constexpr size_t element_to_index(Element element) {
    return static_cast<size_t>(element);
  }

  inline static constexpr Element index_to_element(size_t element_idx) {
    return static_cast<Element>(element_idx);
  }

  Encoding encode(const Element& element) const {
    auto it = m_element_to_encoding.find(element);
    RUNTIME_CHECK(it != m_element_to_encoding.end(), undefined_operation());
    return it->second;
  }

  Element decode(const Encoding& encoding) const {
    auto it = m_encoding_to_element.find(encoding);
    RUNTIME_CHECK(it != m_encoding_to_element.end(), undefined_operation());
    return it->second;
  }

  bool is_bottom(const Encoding& x) const {
    // In the semi-lattice, the Bottom element is the
    // unique bit vector that has only one bit set to 1.
    return x.count() == 1;
  }

  bool is_top(const Encoding& x) const {
    // In the semi-lattice, the Top element is the
    // unique bit vector that has all bits set to 1.
    return x.all();
  }

  bool equals(const Encoding& x, const Encoding& y) const { return x == y; }

  bool geq(const Encoding& x, const Encoding& y) const {
    return equals(meet(x, y), y);
  }

  Encoding meet(const Encoding& x, const Encoding& y) const { return x & y; }

  Encoding bottom() const { return m_bottom; }

  Encoding top() const { return m_top; }

 private:
  // This sanity check verifies that the bitwise And of any two pairs of
  // elements (i.e., the Meet or the Join of those elements depending on the
  // lattice considered) corresponds to an actual element in the lattice.
  // In other words, this procedure makes sure that the input Hasse diagram
  // defines a semi-lattice.
  void sanity_check() {
    // We count the number of bit vectors that have all their bits set to one.
    size_t all_bits_are_set = 0;
    // We count the number of bit vectors that have only one bit set to one.
    size_t one_bit_is_set = 0;
    for (size_t i = 0; i < kCardinality; ++i) {
      Encoding x = m_element_to_encoding[index_to_element(i)];
      if (x.all()) {
        ++all_bits_are_set;
      }
      if (x.count() == 1) {
        ++one_bit_is_set;
      }
      for (size_t j = 0; j < kCardinality; ++j) {
        Encoding y = m_element_to_encoding[index_to_element(j)];
        RUNTIME_CHECK(m_encoding_to_element.find(x & y) !=
                          m_encoding_to_element.end(),
                      internal_error());
      }
    }
    RUNTIME_CHECK(all_bits_are_set == 1 && one_bit_is_set == 1,
                  internal_error()
                      << error_msg("Missing or duplicate extremal element"));
  }

  std::unordered_map<Element, Encoding> m_element_to_encoding;
  std::unordered_map<Encoding, Element> m_encoding_to_element;
  Encoding m_bottom;
  Encoding m_top;
};

} // namespace fad_impl

} // namespace sparta
