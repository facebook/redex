/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FiniteAbstractDomain.h"

#include <gtest/gtest.h>
#include <sstream>

using namespace sparta;

enum Elements { BOTTOM, A, B, C, D, E, TOP };

using Lattice = BitVectorLattice<Elements, 7, std::hash<int>>;

/*
 *              TOP
 *             /   \
 *            D     E
 *           / \   /
 *          B    C
 *           \  /
 *            A
 *            |
 *          BOTTOM
 */
Lattice lattice(
    {BOTTOM, A, B, C, D, E, TOP},
    {{BOTTOM, A}, {A, B}, {A, C}, {B, D}, {C, D}, {C, E}, {D, TOP}, {E, TOP}});

using Domain =
    FiniteAbstractDomain<Elements, Lattice, Lattice::Encoding, &lattice>;

TEST(FiniteAbstractDomainTest, latticeOperations) {
  Domain bottom(BOTTOM);
  Domain a(A);
  Domain b(B);
  Domain c(C);
  Domain d(D);
  Domain e(E);
  Domain top(TOP);

  EXPECT_TRUE(a.equals(Domain(A)));
  EXPECT_FALSE(a.equals(b));
  EXPECT_TRUE(bottom.equals(Domain::bottom()));
  EXPECT_TRUE(top.equals(Domain::top()));
  EXPECT_FALSE(Domain::top().equals(Domain::bottom()));

  EXPECT_TRUE(a.leq(b));
  EXPECT_TRUE(a.leq(e));
  EXPECT_FALSE(b.leq(e));
  EXPECT_TRUE(bottom.leq(top));
  EXPECT_FALSE(top.leq(bottom));

  EXPECT_EQ(A, b.meet(c).element());
  EXPECT_EQ(D, b.join(c).element());
  EXPECT_EQ(C, d.meet(e).element());
  EXPECT_EQ(TOP, d.join(e).element());
  EXPECT_TRUE(d.join(top).is_top());
  EXPECT_TRUE(e.meet(bottom).is_bottom());

  EXPECT_TRUE(b.join(c).equals(b.widening(c)));
  EXPECT_TRUE(b.narrowing(c).equals(b.meet(c)));

  std::ostringstream o1, o2;
  o1 << A;
  o2 << a;
  EXPECT_EQ(o1.str(), o2.str());
}

TEST(FiniteAbstractDomainTest, destructiveOperations) {
  Domain x(E);
  Domain y(B);
  Domain z(C);
  Domain x1 = x;
  Domain y1 = y;
  Domain z1 = z;

  y.join_with(z);
  EXPECT_EQ(D, y.element());
  EXPECT_EQ(C, z.element());
  y.meet_with(x);
  EXPECT_EQ(C, y.element());
  EXPECT_EQ(E, x.element());

  y1.widen_with(z1);
  EXPECT_EQ(D, y1.element());
  EXPECT_EQ(C, z1.element());
  y1.narrow_with(x1);
  EXPECT_EQ(C, y1.element());
  EXPECT_EQ(E, x1.element());

  x.set_to_top();
  EXPECT_TRUE(x.is_top());
  y.set_to_bottom();
  EXPECT_TRUE(y.is_bottom());
  x.set_to_bottom();
  EXPECT_TRUE(x.equals(y));

  x1.meet_with(Domain::bottom());
  EXPECT_TRUE(x1.is_bottom());
  EXPECT_FALSE(z1.is_top());
  y1.join_with(Domain::top());
  EXPECT_TRUE(y1.is_top());
  EXPECT_FALSE(y1.is_bottom());
}

TEST(FiniteAbstractDomainTest, malformedLattice) {
  enum MalformedLatticeElements { bottom, a, b, c, d, top };
  using MalformedLattice =
      BitVectorLattice<MalformedLatticeElements, 6, std::hash<int>>;
  /*
   * This is not a lattice:
   *
   *     top
   *    /   \						\
   *   c     d
   *   |  X  |
   *   a     b
   *    \   /
   *   bottom
   */
  EXPECT_ANY_THROW({
    MalformedLattice malformed_lattice({bottom, a, b, c, d, top},
                                       {{bottom, a},
                                        {bottom, b},
                                        {a, c},
                                        {a, d},
                                        {b, c},
                                        {b, d},
                                        {c, top},
                                        {d, top}});
  });

  /*
   * Two minimal elements:
   *
   *       top
   *      /   \
   *     a     b
   *
   */
  EXPECT_ANY_THROW({
    MalformedLattice malformed_lattice({a, b, top}, {{a, top}, {b, top}});
  });

  /*
   * Two maximal elements:
   *
   *     a     b
   *      \   /
   *     bottom
   *
   */
  EXPECT_ANY_THROW({
    MalformedLattice malformed_lattice({bottom, a, b},
                                       {{bottom, a}, {bottom, b}});
  });
}
