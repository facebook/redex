/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include "DexClass.h"

/**
 * EquivalenceTest verifies that a bytecode transformation does not change
 * semantics by checking that the return value of a given method is the same
 * before and after the transformation is applied.
 *
 * Tests are created by subclassing EquivalenceTest.
 *
 * In order to register the test (so that main() will run it), a global static
 * instance of the test class must be constructed. (Static initialization runs
 * before main().)
 *
 * build_method should populate the bytecode of the dex method. The dex method
 * will have the following signature:
 *
 *   static int before_foo() { ... }
 *
 * generate() will apply the transformation and insert
 *
 *   static int after_foo() { ... }
 *
 * into the test class as well. Then EquivalenceMain.java will assert that
 *
 *   before_foo() == after_foo()
 *
 * TODO: Enable more return types for the test methods!
 */
class EquivalenceTest {
  static std::vector<EquivalenceTest*>& all_tests() {
    // construct on first use to ensure deterministic static init
    static auto* v = new std::vector<EquivalenceTest*>();
    return *v;
  }

 protected:
  EquivalenceTest() { all_tests().push_back(this); }

 public:
  virtual ~EquivalenceTest() = default;
  virtual std::string test_name() = 0;
  virtual void setup(DexClass*) {}
  virtual void build_method(DexMethod*) = 0;
  virtual void transform_method(DexMethod*) = 0;

  void generate(DexClass* cls);
  static void generate_all(DexClass* cls);
};

#define REGISTER_TEST(TestName) static TestName TestName##_singleton;

/**
 * Typically, we'll want to run a number of dex methods as input into a
 * transformation. EQUIVALENCE_TEST expedites this common case.
 *
 * Here, BaseClass must be a child of EquivalenceTest that has supplied an
 * implementation for transform_method. The macro will generate the runtime
 * test name and the static initializer; all that's left is to implement
 * build_method.
 */
#define EQUIVALENCE_TEST(BaseClass, TestName)                        \
  class TestName : public BaseClass {                                \
    virtual std::string test_name() { return #BaseClass #TestName; } \
    virtual void build_method(DexMethod*);                           \
  };                                                                 \
  REGISTER_TEST(TestName);                                           \
  void TestName::build_method
