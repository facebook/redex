/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <AtomicStatCounter.h>

namespace {

template <typename T>
class AtomicStatCounterTest : public ::testing::Test {
 protected:
  using Counter = AtomicStatCounter<T>;

  auto createCounterChecker(const Counter& counter, T expected_value) {
    return [&counter, expected_value]() {
      do {
        // nothing
      } while (counter.load() != expected_value);
    };
  }
};

TYPED_TEST_SUITE_P(AtomicStatCounterTest);

TYPED_TEST_P(AtomicStatCounterTest, ValueInitializationIsValue) {
  const TypeParam value{42};
  const typename TestFixture::Counter counter{value};
  EXPECT_EQ(counter.load(), value);
}

TYPED_TEST_P(AtomicStatCounterTest, CopyConstructorCopies) {
  const typename TestFixture::Counter counter{42};
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const typename TestFixture::Counter copied_counter{counter};
  EXPECT_EQ(counter.load(), copied_counter.load());
}

TYPED_TEST_P(AtomicStatCounterTest, CopyAssignmentCopies) {
  const typename TestFixture::Counter counter{42};
  typename TestFixture::Counter copied_counter{0};
  copied_counter = counter;
  EXPECT_EQ(counter.load(), copied_counter.load());
}

TYPED_TEST_P(AtomicStatCounterTest, MoveConstructorMoves) {
  // In fact move by copying.
  const TypeParam value{42};
  typename TestFixture::Counter counter{value};
  const typename TestFixture::Counter moved_counter{std::move(counter)};
  EXPECT_EQ(moved_counter.load(), value);
}

TYPED_TEST_P(AtomicStatCounterTest, MoveAssignmentMoves) {
  // In fact move by copying.
  const TypeParam value{42};
  typename TestFixture::Counter counter{value};
  typename TestFixture::Counter moved_counter{0};
  moved_counter = std::move(counter);
  EXPECT_EQ(moved_counter.load(), value);
}

TYPED_TEST_P(AtomicStatCounterTest, CastLoads) {
  const TypeParam value{42};
  const typename TestFixture::Counter counter{value};
  EXPECT_EQ(value, static_cast<TypeParam>(counter));
}

TYPED_TEST_P(AtomicStatCounterTest, PostPlusPlusAddsOneReturnsPrev) {
  const TypeParam value{42};
  typename TestFixture::Counter counter{value};
  std::thread checker{this->createCounterChecker(counter, value + 1)};
  EXPECT_EQ(counter++, value);
  EXPECT_EQ(counter.load(), value + 1);
  checker.join();
}

TYPED_TEST_P(AtomicStatCounterTest, PrePlusPlusAddsOneReturnsCurrent) {
  const TypeParam value{42};
  typename TestFixture::Counter counter{value};
  std::thread checker{this->createCounterChecker(counter, value + 1)};
  EXPECT_EQ(++counter, value + 1);
  EXPECT_EQ(counter.load(), value + 1);
  checker.join();
}

TYPED_TEST_P(AtomicStatCounterTest, PlusEqualAddsReturnsCurrent) {
  const TypeParam value{42};
  const TypeParam addend{10};
  typename TestFixture::Counter counter{value};
  std::thread checker{this->createCounterChecker(counter, value + addend)};
  EXPECT_EQ(counter += addend, value + addend);
  EXPECT_EQ(counter.load(), value + addend);
  checker.join();
}

REGISTER_TYPED_TEST_SUITE_P(AtomicStatCounterTest,
                            ValueInitializationIsValue,
                            CopyConstructorCopies,
                            CopyAssignmentCopies,
                            MoveConstructorMoves,
                            MoveAssignmentMoves,
                            CastLoads,
                            PostPlusPlusAddsOneReturnsPrev,
                            PrePlusPlusAddsOneReturnsCurrent,
                            PlusEqualAddsReturnsCurrent);

using AtomicStatCounterTestTypes = ::testing::Types<size_t, int64_t>;
INSTANTIATE_TYPED_TEST_SUITE_P(AtomicStatCounterTests,
                               AtomicStatCounterTest,
                               AtomicStatCounterTestTypes);

// This namespace tests that accidental non-atomic expressions such as counter =
// counter + 1 are disallowed.  This test passes if it compiles.
namespace NoNonAtomicExpressionTest {
// Meta programming technique from
// https://en.cppreference.com/w/cpp/types/void_t.html#Notes
template <typename, typename = void>
struct NonAtomicExpressionAllowed : std::false_type {};
template <typename T>
struct NonAtomicExpressionAllowed<
    T,
    // Error-prone non-atomic expression.
    decltype(std::declval<AtomicStatCounter<T>>() =
                 std::declval<AtomicStatCounter<T>>() + std::declval<T>())>
    : std::true_type {};

static_assert(!NonAtomicExpressionAllowed<size_t>::value,
              "AtomicStatCounter<size_t> = AtomicStatCounter<size_t> + 1 "
              "should not be allowed. "
              "See comments above the definition of AtomicStatCounter.");
static_assert(!NonAtomicExpressionAllowed<int64_t>::value,
              "AtomicStatCounter<int64_t> = AtomicStatCounter<int64_t> + 1 "
              "should not be allowed. "
              "See comments above the definition of AtomicStatCounter.");

} // namespace NoNonAtomicExpressionTest

} // namespace
