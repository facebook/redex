/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <list>
#include <map>
#include <vector>

#include <gmock/gmock.h>

#include <sparta/FlattenIterator.h>

using namespace sparta;

namespace {

template <typename Iterator>
std::vector<typename std::iterator_traits<Iterator>::value_type> collect(
    Iterator begin, Iterator end) {
  std::vector<typename std::iterator_traits<Iterator>::value_type> result;
  for (; begin != end; ++begin) {
    result.push_back(*begin);
  }
  return result;
}

} // namespace

TEST(FlattenIteratorTest, VectorVectorInt) {
  using Vector = std::vector<int>;
  using VectorVector = std::vector<std::vector<int>>;
  using Iterator = FlattenIterator<
      /* OuterIterator */ std::vector<std::vector<int>>::iterator,
      /* InnerIterator */ std::vector<int>::iterator>;

  VectorVector container = {};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            Vector{});

  container = {{1}, {2, 3}, {4, 5, 6}};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{}, {1}, {}, {2, 3}, {}, {4, 5, 6}, {}};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{1}, {}, {2, 3}, {}, {4, 5}, {}, {6}};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            (Vector{1, 2, 3, 4, 5, 6}));
}

TEST(FlattenIteratorTest, ListVectorInt) {
  using Vector = std::vector<int>;
  using ListVector = std::list<std::vector<int>>;
  using Iterator = FlattenIterator<
      /* OuterIterator */ std::list<std::vector<int>>::iterator,
      /* InnerIterator */ std::vector<int>::iterator>;

  ListVector container = {};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            Vector{});

  container = {{1}, {2, 3}, {4, 5, 6}};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{}, {1}, {}, {2, 3}, {}, {4, 5, 6}, {}};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{1}, {}, {2, 3}, {}, {4, 5}, {}, {6}};
  EXPECT_EQ(collect(Iterator(container.begin(), container.end()),
                    Iterator(container.end(), container.end())),
            (Vector{1, 2, 3, 4, 5, 6}));
}

TEST(FlattenIteratorTest, ConstVectorVectorInt) {
  using Vector = std::vector<int>;
  using VectorVector = std::vector<std::vector<int>>;
  using Iterator = FlattenIterator<
      /* OuterIterator */ std::vector<std::vector<int>>::const_iterator,
      /* InnerIterator */ std::vector<int>::const_iterator>;

  VectorVector container = {};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            Vector{});

  container = {{1}, {2, 3}, {4, 5, 6}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{}, {1}, {}, {2, 3}, {}, {4, 5, 6}, {}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{1}, {}, {2, 3}, {}, {4, 5}, {}, {6}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));
}

TEST(FlattenIteratorTest, MapVectorInt) {
  using Vector = std::vector<int>;
  using MapVector = std::map<int, std::vector<int>>;

  struct Dereference {
    static std::vector<int>::const_iterator begin(
        const std::pair<const int, std::vector<int>>& p) {
      return p.second.cbegin();
    }
    static std::vector<int>::const_iterator end(
        const std::pair<const int, std::vector<int>>& p) {
      return p.second.cend();
    }
  };

  using Iterator = FlattenIterator<
      /* OuterIterator */ std::map<int, std::vector<int>>::const_iterator,
      /* InnerIterator */ std::vector<int>::const_iterator,
      Dereference>;

  MapVector container = {};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            Vector{});

  container = {{0, {1}}, {1, {2, 3}}, {3, {4, 5, 6}}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{0, {}}, {1, {1}},       {2, {}}, {3, {2, 3}},
               {4, {}}, {5, {4, 5, 6}}, {6, {}}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{0, {1}},    {1, {}}, {2, {2, 3}}, {3, {}},
               {4, {4, 5}}, {5, {}}, {6, {6}}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));
}

TEST(FlattenIteratorTest, MapListInt) {
  using Vector = std::vector<int>;
  using MapVector = std::map<int, std::list<int>>;

  struct Dereference {
    static std::list<int>::const_iterator begin(
        const std::pair<const int, std::list<int>>& p) {
      return p.second.cbegin();
    }
    static std::list<int>::const_iterator end(
        const std::pair<const int, std::list<int>>& p) {
      return p.second.cend();
    }
  };

  using Iterator = FlattenIterator<
      /* OuterIterator */ std::map<int, std::list<int>>::const_iterator,
      /* InnerIterator */ std::list<int>::const_iterator,
      Dereference>;

  MapVector container = {};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            Vector{});

  container = {{0, {1}}, {1, {2, 3}}, {3, {4, 5, 6}}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{0, {}}, {1, {1}},       {2, {}}, {3, {2, 3}},
               {4, {}}, {5, {4, 5, 6}}, {6, {}}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));

  container = {{0, {1}},    {1, {}}, {2, {2, 3}}, {3, {}},
               {4, {4, 5}}, {5, {}}, {6, {6}}};
  EXPECT_EQ(collect(Iterator(container.cbegin(), container.cend()),
                    Iterator(container.cend(), container.cend())),
            (Vector{1, 2, 3, 4, 5, 6}));
}
