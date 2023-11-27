/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <numeric>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/integer.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <sparta/AbstractDomain.h>
#include <sparta/Exceptions.h>

namespace sparta {

namespace fad_impl {

template <typename Element, size_t kCardinality>
class BitVectorSemiLattice;

template <typename Element, size_t kCardinality>
class BitVectorSemiLatticeCompletion;

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
  static_assert(std::is_base_of_v<LatticeEncoding<Element, Encoding>, Lattice>,
                "Lattice doesn't derive from LatticeEncoding");

  /*
   * A default constructor is required in the AbstractDomain specification.
   */
  FiniteAbstractDomain() : m_encoding(lattice->top()) {}

  explicit FiniteAbstractDomain(const Element& element)
      : m_encoding(lattice->encode(element)) {}

  Element element() const { return lattice->decode(m_encoding); }

  bool is_bottom() const { return lattice->is_bottom(m_encoding); }

  bool is_top() const { return lattice->is_top(m_encoding); }

  bool leq(const FiniteAbstractDomain& other) const {
    return lattice->leq(m_encoding, other.m_encoding);
  }

  bool equals(const FiniteAbstractDomain& other) const {
    return lattice->equals(m_encoding, other.m_encoding);
  }

  void set_to_bottom() { m_encoding = lattice->bottom(); }

  void set_to_top() { m_encoding = lattice->top(); }

  void join_with(const FiniteAbstractDomain& other) {
    m_encoding = lattice->join(m_encoding, other.m_encoding);
  }

  void widen_with(const FiniteAbstractDomain& other) { join_with(other); }

  void meet_with(const FiniteAbstractDomain& other) {
    m_encoding = lattice->meet(m_encoding, other.m_encoding);
  }

  void narrow_with(const FiniteAbstractDomain& other) { meet_with(other); }

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
      : m_opposite_semi_lattice(
            elements, hasse_diagram, /* construct_opposite_lattice */ true),
        m_completion(m_opposite_semi_lattice) {}

  inline Encoding encode(const Element& element) const override {
    return m_opposite_semi_lattice.encode(element);
  }

  inline Element decode(const Encoding& encoding) const override {
    return m_opposite_semi_lattice.decode(encoding);
  }

  inline constexpr bool is_bottom(const Encoding& x) const override {
    return SemiLattice::is_top(x);
  }

  inline constexpr bool is_top(const Encoding& x) const override {
    return SemiLattice::is_bottom(x);
  }

  inline constexpr bool equals(const Encoding& x,
                               const Encoding& y) const override {
    return SemiLattice::equals(x, y);
  }

  inline constexpr bool leq(const Encoding& x,
                            const Encoding& y) const override {
    return SemiLattice::geq(x, y);
  }

  inline constexpr Encoding join(const Encoding& x,
                                 const Encoding& y) const override {
    return SemiLattice::meet(x, y);
  }

  inline Encoding meet(const Encoding& x, const Encoding& y) const override {
    return m_completion.join(x, y);
  }

  inline constexpr Encoding bottom() const override {
    return SemiLattice::top();
  }

  inline constexpr Encoding top() const override {
    return SemiLattice::bottom();
  }

 private:
  SemiLattice m_opposite_semi_lattice;
  fad_impl::BitVectorSemiLatticeCompletion<Element, kCardinality> m_completion;
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

// This is essentially a better std::bitset. It:
//   a) Uses less memory, so a 4 bit encoding uses 1 byte instead of 8.
//   b) Supports comparison and all the usual integer operations.
//   c) Also supports msb/lsb used for decoding, and does so in O(1) time
//      instead of O(bits) time.
//
// Ironically, the only thing it is missing is the only thing std::bitset
// has, which is count (aka, popcnt). But this is easy to emulate.
template <size_t Bits>
using FixedUint =
    boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
        /* MinBits */ Bits,
        /* MaxBits */ Bits,
        boost::multiprecision::unsigned_magnitude,
        boost::multiprecision::unchecked,
        /* Allocator */ void>>;

// N.B.: Prior to Boost 1.79 MinBits and MaxBits were unsigned,
//       not size_t - using auto here papers over this difference.

template <auto MinBits,
          auto MaxBits,
          boost::multiprecision::cpp_integer_type SignType,
          boost::multiprecision::cpp_int_check_type Checked,
          class Allocator>
inline constexpr size_t popcount(
    const boost::multiprecision::number<
        boost::multiprecision::
            cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>>&
        bits) {
  const auto& backend = bits.backend();

  // We use std::bitset::count to access a native popcnt. In principle the limb
  // type could be larger than what a bitset can natively handle, in practice it
  // likely isn't. This all unrolls anyway.
  using BitsetNativeType = unsigned long long;
  constexpr size_t kNativeBits = std::numeric_limits<BitsetNativeType>::digits;
  using LimbType = std::decay_t<decltype(*backend.limbs())>;
  constexpr size_t kLimbBits = std::numeric_limits<LimbType>::digits;

  constexpr size_t kNativesToCount =
      (kLimbBits + kNativeBits - 1) / kNativeBits;
  constexpr size_t kShiftPerNative = kNativesToCount > 1 ? kNativeBits : 0;
  static_assert(kNativesToCount > 0, "bad bit counts");

  size_t result = 0;
  for (size_t i = 0; i != backend.size(); ++i) {
    auto limb_value = backend.limbs()[i];
    for (size_t j = 0; j != kNativesToCount; ++j) {
      const std::bitset<kNativeBits> limb_bitset{BitsetNativeType(limb_value)};
      result += limb_bitset.count();
      limb_value >>= kShiftPerNative;
    }
  }
  return result;
}

template <auto Bits, // Fixed-width only.
          boost::multiprecision::cpp_integer_type SignType,
          boost::multiprecision::cpp_int_check_type Checked,
          class Allocator>
inline constexpr size_t count_leading_zeros(
    const boost::multiprecision::number<
        boost::multiprecision::
            cpp_int_backend<Bits, Bits, SignType, Checked, Allocator>>& bits) {
  return Bits - (bits.is_zero() ? 0 : (1 + msb(bits)));
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
 * ----------------------------------------------------------------------------
 *
 * In the other direction, we must decode a bitvector back to its element. To
 * do this efficiently, we observe that any poset (and therefore lattice) can
 * be relabelled so that the resulting bitmatrix is anti-triangular. That is,
 * where every encoding has a unique number of leading zeros; this gives each
 * encoding an identity that we can cheaply compute.
 *
 * This is a linear extension done by topological sort. Since we already have
 * the transitive closure, we can simply sort each encoding by popcount to
 * retrieve a topological ordering. The bottom element is relabelled with a
 * value of zero and moved to the 1st bit (the LSB). Then the next smallest is
 * placed at the 2nd bit, and so on until the top element occupies the MSB.
 *
 * Since we have a reflexive bit, within such a topological relabelling each
 * element will be uniquely the only one that sets its own reflexive bit and
 * sets no other more significant bits (i.e., topologically greater elements).
 * This establishes a unique leading zeros count.
 *
 * For example:
 *
 *  Input                                      Relabelled
 *    0       MSB     LSB      Topo-Sort           4'          MSB     LSB
 *    |     0: 1 1 1 1 1        2 -> 0'            |        0': 0 0 0 0 1
 *    1     1: 1 1 1 1 0        3 -> 1'            3'       1': 0 0 0 1 1
 *   / \    2: 0 0 1 0 0        4 -> 2'           / \       2': 0 0 1 0 1
 *  3   4   3: 0 1 1 0 0        1 -> 3'          1'  2'     3': 0 1 1 1 1
 *   \ /    4: 1 0 1 0 0        0 -> 4'           \ /       4': 1 1 1 1 1
 *    2                                            0'
 *
 * To decode, we count the leading zeros and then use this to index a table
 * tracking which element mapped to that encoding. One could flip this so that
 * smaller elements go last; the relabelled matrix would be triangular and we
 * would count trailing zeros - but counting leading zeros is slightly faster.
 *
 * If we wanted to be ultra-strict we could require that the elements are
 * already labelled such that no permutation needs to occur, then we could cast
 * the clz to the element directly - benchmarks show this is not any faster than
 * a lookup table, so (thankfully) we don't need to be that annoying.
 */
template <typename Element, size_t kCardinality>
class BitVectorSemiLattice final {
 public:
  static_assert(kCardinality >= 2, "Lattice must have at least 2 elements.");

  using Encoding = FixedUint<kCardinality>;

  // The bottom element is always one because it topologically sorts to the
  // smallest element, so permutes to the LSB. We do not need to track it.
  static constexpr Encoding kBottom = Encoding(1);

  // The top element is always all ones, by definition.
  static constexpr Encoding kTop = Encoding(-1);

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
    SPARTA_RUNTIME_CHECK(elements.size() == kCardinality,
                         invalid_argument()
                             << argument_name("elements")
                             << operation_name("BitVectorSemiLattice()"));
    SPARTA_RUNTIME_CHECK(is_zero_based_integer_range(elements),
                         invalid_argument()
                             << argument_name("elements")
                             << operation_name("BitVectorSemiLattice()"));

    // We populate the Boolean matrix by traversing the Hasse diagram of the
    // partial order.
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
      bit_set(m_index_to_encoding[y_idx], x_idx);
    }

    // We first compute the reflexive closure of the "immediately greater than"
    // relation in the lattice considered.
    for (size_t i = 0; i < kCardinality; ++i) {
      bit_set(m_index_to_encoding[i], i);
    }

    // Then we compute the transitive closure of the "immediately greater than"
    // relation in the lattice considered, using Warshall's algorithm.
    for (size_t k = 0; k < kCardinality; ++k) {
      for (size_t i = 0; i < kCardinality; ++i) {
        for (size_t j = 0; j < kCardinality; ++j) {
          if (bit_test(m_index_to_encoding[i], k) &&
              bit_test(m_index_to_encoding[k], j)) {
            bit_set(m_index_to_encoding[i], j);
          }
        }
      }
    }

    // Determine the relabelling for each element. We do this in terms of the
    // number of leading zeros the relabelled encoding will have, rather than
    // just in terms of the relabelled value; that way this can be reused for
    // the final clz to index mapping as-is.
    //
    // As such, we place the elements with lower popcount last (i.e., they
    // relabel to smaller values and so have higher leading zero counts).
    std::iota(m_clz_to_index.begin(), m_clz_to_index.end(), 0);
    std::sort(m_clz_to_index.begin(),
              m_clz_to_index.end(),
              [&](size_t first_index, size_t second_index) {
                const auto first_popcount =
                    popcount(m_index_to_encoding[first_index]);
                const auto second_popcount =
                    popcount(m_index_to_encoding[second_index]);
                // Break ties by element value (i.e., index).
                return std::tie(first_popcount, first_index) >
                       std::tie(second_popcount, second_index);
              });

    // Permute the encodings with the new relabelling so that each element has
    // an encoding uniquely identified by the number of leading zeros. This
    // necessarily means that bottom is assigned to the least significant bit
    // and encodes to all but one leading zero; and top is assigned the most
    // significant bit and encodes to no leading zeros (all ones).
    //
    // We don't permute rows, which would give the matrix as if the element
    // values had been originally as determined for relabelling. This way we
    // don't need to perform an input index to permuted index mapping just to
    // then lookup up permuted index to permuted encoding:
    //
    //      MSB     LSB   Relabelling      MSB     LSB    Actual Storage
    //    0: 1 1 1 1 1       2->0'      0': 0 0 0 0 1      0: 1 1 1 1 1
    //    1: 1 1 1 1 0       3->1'      1': 0 0 0 1 1      1: 0 1 1 1 1
    //    2: 0 0 1 0 0       4->2'      2': 0 0 1 0 1      2: 0 0 0 0 1
    //    3: 0 1 1 0 0       1->3'      3': 0 1 1 1 1      3: 0 0 0 1 1
    //    4: 1 0 1 0 0       0->4'      4': 1 1 1 1 1      4: 0 0 1 0 1
    //
    for (size_t i = 0; i != kCardinality; ++i) {
      const auto original = m_index_to_encoding[i];
      for (size_t clz = 0; clz != kCardinality; ++clz) {
        const auto original_index = m_clz_to_index[clz];
        const auto relabelled_index = kCardinality - clz - 1;
        if (bit_test(original, original_index)) {
          bit_set(m_index_to_encoding[i], relabelled_index);
        } else {
          bit_unset(m_index_to_encoding[i], relabelled_index);
        }
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

  inline const Encoding& encode(const Element& element) const {
    const auto element_idx = element_to_index(element);
    SPARTA_RUNTIME_CHECK(element_idx < m_index_to_encoding.size(),
                         undefined_operation() << error_msg("Invalid element"));
    return m_index_to_encoding[element_idx];
  }

  inline Element decode(const Encoding& encoding) const {
    const auto encoding_clz = count_leading_zeros(encoding);
    SPARTA_RUNTIME_CHECK(encoding_clz < m_clz_to_index.size(),
                         undefined_operation()
                             << error_msg("Invalid encoding"));
    return index_to_element(m_clz_to_index[encoding_clz]);
  }

  inline static constexpr bool is_bottom(const Encoding& x) {
    // The bottom element is the unique element with one bit set. However,
    // we do not need to do a full popcount or equality compare; checking if it
    // has the largest leading zero count is O(1) instead of O(bits).
    return count_leading_zeros(x) == kCardinality - 1;
  }

  inline static constexpr bool is_top(const Encoding& x) {
    // The top element is the unique element with all bits set. However,
    // we do not need to do a full popcount or equality compare; checking if it
    // has no leading zeros is O(1) instead of O(bits).
    return count_leading_zeros(x) == 0;
  }

  inline static constexpr bool equals(const Encoding& x, const Encoding& y) {
    return x == y;
  }

  inline static constexpr bool geq(const Encoding& x, const Encoding& y) {
    return equals(meet(x, y), y);
  }

  inline static constexpr Encoding meet(const Encoding& x, const Encoding& y) {
    return x & y;
  }

  inline static constexpr const Encoding& bottom() { return kBottom; }

  inline static constexpr const Encoding& top() { return kTop; }

 private:
  // We can use a smaller integer type than size_t to store element indices
  // in the lookup table. This makes both tables often fit in one cache line.
  using Index = typename boost::uint_value_t<kCardinality - 1>::fast;

  // This sanity check verifies that the bitwise And of any two pairs of
  // elements (i.e., the Meet or the Join of those elements depending on the
  // lattice considered) corresponds to an actual element in the lattice.
  // In other words, this procedure makes sure that the input Hasse diagram
  // defines a semi-lattice.
  void sanity_check() {
    // We verify that only one element has all bits set.
    bool found_all_bits_are_set = false;
    // We verify that only one element has one bit set.
    bool found_one_bit_is_set = false;
    // We verify each leading zero count appears exactly once.
    std::bitset<kCardinality> found_clz;

    for (size_t i = 0; i < kCardinality; ++i) {
      const auto x_element = index_to_element(i);
      const auto x = encode(x_element);

      const auto x_popcount = popcount(x);
      if (x_popcount == kCardinality) {
        SPARTA_RUNTIME_CHECK(
            !found_all_bits_are_set,
            internal_error() << error_msg("Duplicate top element encoding"));
        found_all_bits_are_set = true;
      } else if (x_popcount == 1) {
        SPARTA_RUNTIME_CHECK(
            !found_one_bit_is_set,
            internal_error() << error_msg("Duplicate bottom element encoding"));
        found_one_bit_is_set = true;
      }

      const auto x_clz = count_leading_zeros(x);
      SPARTA_RUNTIME_CHECK(
          x_clz < kCardinality,
          internal_error() << error_msg("Out of range leading zeros count"));
      SPARTA_RUNTIME_CHECK(!found_clz[x_clz],
                           internal_error()
                               << error_msg("Duplicate leading zeros count"));
      found_clz[x_clz] = true;

      if (x_popcount == kCardinality) {
        SPARTA_RUNTIME_CHECK(x_clz == 0,
                             internal_error() << error_msg(
                                 "Top element encoding has leading zeros"));
      } else if (x_popcount == 1) {
        SPARTA_RUNTIME_CHECK(
            x_clz == kCardinality - 1,
            internal_error()
                << error_msg("Bottom element encoding missing leading zeros"));
      }

      SPARTA_RUNTIME_CHECK(decode(x) == x_element,
                           internal_error() << error_msg("Incorrect decoding"));

      for (size_t j = 0; j < kCardinality; ++j) {
        const auto y_element = index_to_element(j);
        const auto y = encode(y_element);
        SPARTA_RUNTIME_CHECK((x_element == y_element) == equals(x, y),
                             internal_error()
                                 << error_msg("Incorrect encoding equality"));

        const auto x_meet_y = meet(x, y);
        const auto x_meet_y_iter = std::find(
            m_index_to_encoding.begin(), m_index_to_encoding.end(), x_meet_y);
        SPARTA_RUNTIME_CHECK(x_meet_y_iter != m_index_to_encoding.end(),
                             internal_error()
                                 << error_msg("Meet element encoding missing"));

        const auto x_meet_y_index = x_meet_y_iter - m_index_to_encoding.begin();
        const auto x_meet_y_element = index_to_element(x_meet_y_index);
        SPARTA_RUNTIME_CHECK(
            decode(x_meet_y) == x_meet_y_element,
            internal_error() << error_msg("Meet encoding invalid decoding"));
      }
    }
    SPARTA_RUNTIME_CHECK(found_all_bits_are_set,
                         internal_error()
                             << error_msg("Missing top element encoding"));
    SPARTA_RUNTIME_CHECK(found_one_bit_is_set,
                         internal_error()
                             << error_msg("Missing bottom element encoding"));
    SPARTA_RUNTIME_CHECK(
        found_clz.all(),
        internal_error() << error_msg("Missing leading zeros count encoding"));
  }

  std::array<Encoding, kCardinality> m_index_to_encoding;
  std::array<Index, kCardinality> m_clz_to_index;
};

/*
 * Completes a semi-lattice by providing a join operation.
 *
 * We could construct a second BitVectorSemiLattice with the reversed ordering.
 * Then to join X and Y we could perform the series of transforms:
 *     - Count leading zeros of Enc(X) and Enc(Y).
 *     - Use these to lookup the elements X and Y.
 *     - Look up Enc'(X) and Enc'(Y) in the reversed lattice.
 *     - Perform the meet, giving Enc'(Z).
 *     - Count leading zeros of Enc'(Z).
 *     - Use this to lookup the element Z.
 *     - Loop up Enc(Z) in the original lattice.
 *
 * This can be done faster by observing that after counting leading zeros, we
 * have identified the element in some relabelled space. We can jump directly
 * to its reversed encoding without needing to know the original elements:
 *     - Count leading zeros of Enc(X) and Enc(Y).
 *     - Use these to lookup Enc'(X) and Enc'(Y) directly.
 *     - Perform the meet, giving Enc'(Z).
 *     - Count leading zeros of Enc'(Z).
 *     - Use this to lookup Enc(Z) directly.
 */
template <typename Element, size_t kCardinality>
class BitVectorSemiLatticeCompletion final {
  using SemiLattice = BitVectorSemiLattice<Element, kCardinality>;
  using Encoding = typename SemiLattice::Encoding;

  struct ReversedEncoding { // To prevent mix-ups.
    Encoding inner;
  };

 public:
  explicit BitVectorSemiLatticeCompletion(const SemiLattice& semi) {
    // We don't need to perform the same from-scratch construction as the
    // semi-lattice. The transpose of the encoding matrix gives us the
    // reversed ordering, and the property of having a unique leading zeros
    // count is preserved:
    //
    //    Relabelling      MSB     LSB   Reversed      MSB     LSB
    //       2->0'      0': 0 0 0 0 1     2->4"     0": 0 0 0 0 1
    //       3->1'      1': 0 0 0 1 1     3->3"     1": 0 0 0 1 1
    //       4->2'      2': 0 0 1 0 1     4->2"     2": 0 0 1 1 1
    //       1->3'      3': 0 1 1 1 1     1->1"     3": 0 1 0 1 1
    //       0->4'      4': 1 1 1 1 1     0->0"     4": 1 1 1 1 1
    //
    // The topological order is simply reversed: the clz of an element in its
    // (non-reversed) encoding gives the relabelling in the reversed ordering.
    // E.g., in the above element 2 is bottom, so relabels to 0' and is given
    // an encoding with clz=4. Therefore, in the reversed encoding it is top
    // so relabels to 4" and is given an encoding with clz=0.
    for (size_t element_idx = 0; element_idx < kCardinality; ++element_idx) {
      const auto element = SemiLattice::index_to_element(element_idx);
      const auto& encoding = semi.encode(element);
      const auto encoding_clz = count_leading_zeros(encoding);
      SPARTA_RUNTIME_CHECK(
          encoding_clz < kCardinality,
          internal_error() << error_msg("Out of range leading zeros count"));

      // The actual row this encoding would be at in the relabelled matrix.
      const auto relabelled_index = kCardinality - encoding_clz - 1;

      // To transpose, bit i of the encoding goes in row kCardinality - i - 1,
      // at column kCardinality - relabelled_index - 1. This column value is
      // actually just encoding_clz again.
      for (size_t i = 0; i < kCardinality; ++i) {
        auto& transpose_row = m_clz_to_reversed_encoding[kCardinality - i - 1];
        if (bit_test(encoding, i)) {
          bit_set(transpose_row.inner, encoding_clz);
        }
      }

      // What this element was relabelled to is the same as what the reversed
      // encoding's clz will end up being for this element. This lets us jump
      // back to the non-reversed encoding given an reversed encoding.
      m_reversed_clz_to_encoding[relabelled_index] = encoding;
    }

    sanity_check(semi);
  }

  inline Encoding join(const Encoding& x, const Encoding& y) const {
    const auto& x_reversed = to_reversed(x);
    const auto& y_reversed = to_reversed(y);
    const ReversedEncoding z_reversed{
        SemiLattice::meet(x_reversed.inner, y_reversed.inner)};
    return from_reversed(z_reversed);
  }

 private:
  const ReversedEncoding& to_reversed(const Encoding& encoding) const {
    const auto encoding_clz = count_leading_zeros(encoding);
    SPARTA_RUNTIME_CHECK(encoding_clz < m_clz_to_reversed_encoding.size(),
                         undefined_operation()
                             << error_msg("Invalid encoding"));
    return m_clz_to_reversed_encoding[encoding_clz];
  }

  const Encoding& from_reversed(const ReversedEncoding& encoding) const {
    const auto encoding_clz = count_leading_zeros(encoding.inner);
    SPARTA_RUNTIME_CHECK(encoding_clz < m_reversed_clz_to_encoding.size(),
                         undefined_operation()
                             << error_msg("Invalid encoding"));
    return m_reversed_clz_to_encoding[encoding_clz];
  }

  void sanity_check(const SemiLattice& semi) {
    // The semi lattice sanity check did most of the heavy lifting. But we can
    // still verify the new lookup table is correct for all mappings.
    std::bitset<kCardinality> found_reversed_clz;
    for (size_t i = 0; i < kCardinality; ++i) {
      const auto x_element = SemiLattice::index_to_element(i);
      const auto& x = semi.encode(x_element);
      const auto& x_reversed = to_reversed(x);

      const auto x_reversed_clz = count_leading_zeros(x_reversed.inner);
      SPARTA_RUNTIME_CHECK(
          x_reversed_clz < kCardinality,
          internal_error() << error_msg("Out of range leading zeros count"));
      SPARTA_RUNTIME_CHECK(!found_reversed_clz[x_reversed_clz],
                           internal_error()
                               << error_msg("Duplicate leading zeros count"));
      found_reversed_clz[x_reversed_clz] = true;

      SPARTA_RUNTIME_CHECK(
          from_reversed(x_reversed) == x,
          internal_error() << error_msg("Incorrect reverse encoding mapping"));

      for (size_t j = 0; j < kCardinality; ++j) {
        const auto y_element = SemiLattice::index_to_element(j);
        const auto& y = semi.encode(y_element);

        const auto x_join_y = join(x, y);
        SPARTA_RUNTIME_CHECK(semi.geq(x_join_y, x),
                             internal_error()
                                 << error_msg("Join element out of order"));
        SPARTA_RUNTIME_CHECK(semi.geq(x_join_y, y),
                             internal_error()
                                 << error_msg("Join element out of order"));
      }
    }
    SPARTA_RUNTIME_CHECK(
        found_reversed_clz.all(),
        internal_error() << error_msg("Missing leading zeros count encoding"));
  }

  std::array<ReversedEncoding, kCardinality> m_clz_to_reversed_encoding;
  std::array<Encoding, kCardinality> m_reversed_clz_to_encoding;
};

} // namespace fad_impl

} // namespace sparta
