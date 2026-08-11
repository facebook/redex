/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"
#include "Debug.h"

#include <algorithm>
#include <boost/thread.hpp> // NOLINT(facebook-unused-include-check): boost::thread is used
#include <chrono>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

constexpr size_t kThreads = 50;
constexpr size_t kSampleSize = 1000;

class ConcurrentContainersTest : public ::testing::Test {
 protected:
  ConcurrentContainersTest()
      : m_generator(std::random_device()()),
        m_size(kSampleSize),
        m_elem_dist(0, 1000000000),
        m_data(generate_random_data()),
        m_subset_data(generate_random_subset(m_data)),
        m_data_set(m_data.begin(), m_data.end()),
        m_subset_data_set(m_subset_data.begin(), m_subset_data.end()) {
    for (size_t t = 0; t < kThreads; ++t) {
      for (size_t i = t; i < m_data.size(); i += kThreads) {
        m_samples[t].push_back(m_data[i]);
      }
      for (size_t i = t; i < m_subset_data.size(); i += kThreads) {
        m_subset_samples[t].push_back(m_subset_data[i]);
      }
    }
  }

  std::vector<uint32_t> generate_random_data() {
    std::vector<uint32_t> s;
    s.reserve(m_size);
    for (size_t i = 0; i < m_size; ++i) {
      s.push_back(m_elem_dist(m_generator));
    }
    return s;
  }

  std::vector<uint32_t> generate_random_subset(
      const std::vector<uint32_t>& data) {
    auto new_data = data;
    unsigned seed = static_cast<unsigned>(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::shuffle(
        new_data.begin(), new_data.end(), std::default_random_engine(seed));
    new_data.erase(new_data.begin(), new_data.begin() + m_size / 2);
    return new_data;
  }

  void run_on_samples(
      const std::vector<uint32_t> samples[],
      const std::function<void(const std::vector<uint32_t>&)>& operation) {
    std::vector<boost::thread> threads;
    for (size_t t = 0; t < kThreads; ++t) {
      const auto& sample = samples[t];
      threads.emplace_back([&sample, operation]() { operation(sample); });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }

  void run_on_samples(
      const std::function<void(const std::vector<uint32_t>&)>& operation) {
    run_on_samples(m_samples, operation);
  }

  void run_on_subset_samples(
      const std::function<void(const std::vector<uint32_t>&)>& operation) {
    run_on_samples(m_subset_samples, operation);
  }

  std::mt19937 m_generator;
  uint32_t m_size;
  std::uniform_int_distribution<uint32_t> m_elem_dist;
  std::vector<uint32_t> m_data;
  std::vector<uint32_t> m_subset_data;
  std::unordered_set<uint32_t> m_data_set;
  std::unordered_set<uint32_t> m_subset_data_set;
  std::vector<uint32_t> m_samples[kThreads];
  std::vector<uint32_t> m_subset_samples[kThreads];
};

TEST_F(ConcurrentContainersTest, concurrentSetTest) {
  ConcurrentSet<uint32_t> set;

  run_on_samples([&set](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      set.insert(sample[i]);
      EXPECT_EQ(1, set.count(sample[i]));
    }
  });
  EXPECT_EQ(m_data_set.size(), set.size());
  auto check_initial_values = [&](const ConcurrentSet<uint32_t>& set) {
    for (uint32_t x : m_data) {
      EXPECT_EQ(1, set.count(x));
      EXPECT_NE(set.end(), set.find(x));
    }
  };
  check_initial_values(set);

  auto copy = set;

  run_on_subset_samples([&set](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      set.erase(sample[i]);
    }
  });

  for (uint32_t x : m_subset_data) {
    EXPECT_EQ(0, set.count(x));
    EXPECT_EQ(set.end(), set.find(x));
  }

  run_on_samples([&set](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      set.erase(sample[i]);
    }
  });
  EXPECT_EQ(0, set.size());
  for (uint32_t x : m_data) {
    EXPECT_EQ(0, set.count(x));
    EXPECT_EQ(set.end(), set.find(x));
  }

  // Check that copy is unchanged.
  check_initial_values(copy);

  auto moved = std::move(copy);
  check_initial_values(moved);

  set.insert({1, 2, 3});
  EXPECT_EQ(3, set.size());
  set.clear();
  EXPECT_EQ(0, set.size());

  std::unordered_set<uint32_t> non_concurrent_set{1, 5, 7, 9};
  set.insert(non_concurrent_set.begin(), non_concurrent_set.end());
  EXPECT_EQ(4, set.size());
  set.clear();
  EXPECT_EQ(0, set.size());
}

// End-to-end wrapper check: iterating a populated ConcurrentSet (via
// UnorderedIterable, which concatenates its shards) visits every inserted
// element exactly once and skips none, regardless of perturbation. Confirms the
// public wrapper composes the shard tables (perturbed under
// REDEX_PERTURB_UNORDERED) correctly.
TEST_F(ConcurrentContainersTest, unorderedIterableVisitsEveryElementOnce) {
  ConcurrentSet<uint32_t> set;
  run_on_samples([&set](const std::vector<uint32_t>& sample) {
    for (uint32_t x : sample) {
      set.insert(x);
    }
  });
  ASSERT_EQ(m_data_set.size(), set.size());

  std::unordered_set<uint32_t> seen;
  size_t count = 0;
  for (uint32_t x : UnorderedIterable(set)) {
    EXPECT_TRUE(seen.insert(x).second) << "element visited twice: " << x;
    ++count;
  }
  EXPECT_EQ(set.size(), count);
  EXPECT_EQ(m_data_set, seen);
}

TEST_F(ConcurrentContainersTest, insertOnlyConcurrentSetTest) {
  InsertOnlyConcurrentSet<uint32_t> set;

  run_on_subset_samples([&set](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      set.insert(sample[i]);
      EXPECT_EQ(1, set.count(sample[i]));
    }
  });
  auto check_initial_values =
      [&](const InsertOnlyConcurrentSet<uint32_t>& set) {
        EXPECT_EQ(m_subset_data_set.size(), set.size());
        for (uint32_t x : m_subset_data) {
          EXPECT_EQ(1, set.count(x));
          EXPECT_NE(set.end(), set.find(x));
          EXPECT_NE(nullptr, set.get(x));
        }
      };
  check_initial_values(set);

  auto copy = set;

  run_on_samples([&set](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      set.insert(sample[i]);
      EXPECT_EQ(1, set.count(sample[i]));
    }
  });

  for (uint32_t x : m_data) {
    EXPECT_EQ(1, set.count(x));
    EXPECT_NE(set.end(), set.find(x));
    EXPECT_NE(nullptr, set.get(x));
  }

  // Check that copy is unchanged.
  check_initial_values(copy);

  auto moved = std::move(copy);
  check_initial_values(moved);

  // Check that pointers/references are stable.
  struct Pair {
    const uint32_t* p;
    uint32_t x;
  };
  std::vector<Pair> pointers;

  for (uint32_t x : m_subset_data) {
    const uint32_t* p = moved.insert(x).first;
    ASSERT_NE(nullptr, p);
    // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
    EXPECT_EQ(*p, x);
    pointers.push_back(Pair{p, x});
  }
  EXPECT_EQ(m_subset_data_set.size(), moved.size());

  run_on_samples([&moved](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      moved.insert(sample[i]);
      EXPECT_EQ(1, moved.count(sample[i]));
    }
  });
  EXPECT_EQ(m_data_set.size(), moved.size());

  for (const auto& pair : pointers) {
    ASSERT_NE(nullptr, pair.p);
    // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
    EXPECT_EQ(*pair.p, pair.x);
    EXPECT_EQ(pair.p, moved.insert(pair.x).first);
    EXPECT_EQ(pair.p, moved.get(pair.x));
  }
}

TEST_F(ConcurrentContainersTest, concurrentMapTest) {
  ConcurrentMap<std::string, uint32_t> map;

  run_on_samples([&map](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      std::string s = std::to_string(sample[i]);
      map.insert({s, sample[i]});
      EXPECT_EQ(1, map.count(s));
    }
  });
  EXPECT_EQ(m_data_set.size(), map.size());
  for (uint32_t x : m_data) {
    std::string s = std::to_string(x);
    EXPECT_EQ(1, map.count(s));
    auto it = map.find(s);
    EXPECT_NE(map.end(), it);
    EXPECT_EQ(s, it->first);
    EXPECT_EQ(x, it->second);
  }

  std::unordered_map<uint32_t, size_t> occurrences;
  for (uint32_t x : m_data) {
    ++occurrences[x];
  }
  run_on_samples([&map](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      std::string s = std::to_string(sample[i]);
      map.update(
          s, [&s](const std::string& key, uint32_t& value, bool key_exists) {
            EXPECT_EQ(s, key);
            EXPECT_TRUE(key_exists);
            ++value;
          });
    }
  });
  EXPECT_EQ(m_data_set.size(), map.size());
  auto check_initial_values =
      [&](const ConcurrentMap<std::string, uint32_t>& map) {
        for (uint32_t x : m_data) {
          std::string s = std::to_string(x);
          EXPECT_EQ(1, map.count(s));
          auto it = map.find(s);
          EXPECT_NE(map.end(), it);
          EXPECT_EQ(s, it->first);
          EXPECT_EQ(x + occurrences[x], it->second);
        }
      };
  check_initial_values(map);

  auto copy = map;

  run_on_subset_samples([&map](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      auto* p = map.get_and_erase(std::to_string(sample[i]));
      EXPECT_TRUE(p);
      EXPECT_EQ(*p, sample[i] + 1);
    }
  });

  for (uint32_t x : m_subset_data) {
    std::string s = std::to_string(x);
    EXPECT_EQ(0, map.count(s));
    EXPECT_EQ(map.end(), map.find(s));
  }

  run_on_samples([&map](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      map.erase(std::to_string(sample[i]));
    }
  });
  EXPECT_EQ(0, map.size());
  for (uint32_t x : m_data) {
    std::string s = std::to_string(x);
    EXPECT_EQ(0, map.count(s));
    EXPECT_EQ(map.end(), map.find(s));
  }

  // Check that copy is unchanged.
  check_initial_values(copy);

  auto moved = std::move(copy);
  check_initial_values(moved);

  map.insert({{"a", 1}, {"b", 2}, {"c", 3}});
  EXPECT_EQ(3, map.size());
  map.clear();
  EXPECT_EQ(0, map.size());
}

TEST_F(ConcurrentContainersTest, insertOnlyConcurrentMapTest) {
  InsertOnlyConcurrentMap<std::string, uint32_t> map;

  InsertOnlyConcurrentMap<std::string, const uint32_t*> ptrs;
  run_on_samples([&map, &ptrs](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      std::string s = std::to_string(sample[i]);
      if (i % 3 == 0) {
        map.insert({s, sample[i]});
        ptrs.emplace(s, map.get(s));
      } else if (i % 3 == 1) {
        const auto* ptr =
            map.get_or_create_and_assert_equal(
                   s, [](const auto& t) { return (uint32_t)atoi(t.c_str()); })
                .first;
        ptrs.emplace(s, ptr);
      } else {
        const auto* ptr =
            map.get_or_emplace_and_assert_equal(s, sample[i]).first;
        ptrs.emplace(s, ptr);
      }
      EXPECT_EQ(1, map.count(s));
    }
  });

  run_on_samples([&map, &ptrs](const std::vector<uint32_t>& sample) {
    for (size_t i = 0; i < sample.size(); ++i) {
      std::string s = std::to_string(sample[i]);
      auto [ptr1, emplaced] = map.get_or_emplace_and_assert_equal(s, sample[i]);
      EXPECT_FALSE(emplaced);
      auto [ptr2, created] = map.get_or_create_and_assert_equal(
          s, [](const auto& /*t*/) -> uint32_t { not_reached(); });
      EXPECT_FALSE(created);
      EXPECT_EQ(ptrs.at(s), ptr1);
      EXPECT_EQ(ptr1, ptr2);
    }
  });
  EXPECT_EQ(m_data_set.size(), map.size());
  for (uint32_t x : m_data) {
    std::string s = std::to_string(x);
    EXPECT_EQ(1, map.count(s));
    auto it = map.find(s);
    EXPECT_NE(map.end(), it);
    EXPECT_EQ(s, it->first);
    EXPECT_EQ(x, it->second);
    const auto* p = ptrs.at(s);
    EXPECT_EQ(p, map.get(s));
    EXPECT_EQ(p, map.get_unsafe(s));
  }

  EXPECT_EQ(m_data_set.size(), map.size());
}

// Regression: the rvalue-key overload of get_or_create_and_assert_equal must
// not move `key` into the creator and then again into the map. A creator that
// consumes its argument by value would otherwise leave a moved-from key stored.
TEST_F(ConcurrentContainersTest, getOrCreateRvalueKeyIsNotMovedFrom) {
  InsertOnlyConcurrentMap<std::string, size_t> map;
  // Long enough to defeat small-string optimization, so a moved-from husk is
  // reliably distinct from the original key.
  const std::string original = "a_reasonably_long_key_that_avoids_sso_buffer";

  std::string key = original;
  auto [ptr, created] =
      map.get_or_create_and_assert_equal(std::move(key), [](std::string k) {
        // Genuinely consume the key by move: this models a consuming creator
        // (a const-ref parameter would not reproduce the pre-fix double-move)
        // and avoids performance-unnecessary-value-param.
        const std::string consumed = std::move(k);
        return consumed.size();
      });

  EXPECT_TRUE(created);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(original.size(), *ptr); // sanity check; the value is correct even
                                    // with the bug (the creator sees the key
                                    // before it is stored).
  // These are the assertions that distinguish buggy from fixed: with the
  // double-move the stored key is a moved-from husk, so a lookup of the
  // original key fails.
  EXPECT_EQ(1, map.count(original));
  auto it = map.find(original);
  ASSERT_NE(map.end(), it);
  EXPECT_EQ(original, it->first);
}

TEST_F(ConcurrentContainersTest, move) {
  ConcurrentMap<void*, void*> map1;
  map1.emplace(nullptr, nullptr);
  EXPECT_EQ(1, map1.size());
  auto map2 = std::move(map1);
  EXPECT_EQ(1, map2.size());
  map1 = std::move(map2);
  EXPECT_EQ(1, map1.size());
}

TEST_F(ConcurrentContainersTest, copy) {
  ConcurrentMap<void*, void*> map1;
  map1.emplace(nullptr, nullptr);
  EXPECT_EQ(1, map1.size());
  auto map2 = map1;
  EXPECT_EQ(1, map1.size());
  EXPECT_EQ(1, map2.size());
}

TEST_F(ConcurrentContainersTest, insert_or_assign) {
  ConcurrentMap<uint32_t, std::unique_ptr<uint32_t>> map;
  run_on_samples([&map](const std::vector<uint32_t>& sample) {
    for (auto x : sample) {
      map.insert_or_assign(std::make_pair(x, std::make_unique<uint32_t>(x)));
    }
  });
  EXPECT_EQ(m_data_set.size(), map.size());
  for (uint32_t x : m_data) {
    EXPECT_TRUE(map.count(x));
    auto& p = map.at_unsafe(x);
    ASSERT_TRUE(p);
    // @lint-ignore NULLSAFECLANG (guarded by ASSERT_TRUE above)
    EXPECT_EQ(x, *p);
  }

  run_on_samples([&map](const std::vector<uint32_t>& sample) {
    for (auto x : sample) {
      map.insert_or_assign(
          std::make_pair(x, std::make_unique<uint32_t>(x + 1)));
    }
  });
  EXPECT_EQ(m_data_set.size(), map.size());
  for (uint32_t x : m_data) {
    EXPECT_TRUE(map.count(x));
    auto& p = map.at_unsafe(x);
    ASSERT_TRUE(p);
    // @lint-ignore NULLSAFECLANG (guarded by ASSERT_TRUE above)
    EXPECT_EQ(x + 1, *p);
  }
}

TEST_F(ConcurrentContainersTest, atThrows) {
  ConcurrentMap<void*, void*> empty;
  bool threw = false;
  try {
    empty.at(nullptr);
  } catch (const std::out_of_range&) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

TEST_F(ConcurrentContainersTest, iteratorPostIncrementReturnsIndependentCopy) {
  using Hashtable = cc_impl::
      ConcurrentHashtable<int, int, std::hash<int>, std::equal_to<int>>;
  Hashtable table;

  table.try_insert(100);
  table.try_insert(200);

  auto it = table.begin();
  auto old = it++;

  EXPECT_NE(it, old);
}
