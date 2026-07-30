/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "DexClass.h"
#include "JarLoader.h"
#include "RedexTest.h"

namespace {

// Minimal big-endian class-file builder. The JarLoader reads multi-byte values
// in network byte order, so everything is emitted big-endian here.
struct ClassFileBuilder {
  std::vector<uint8_t> bytes;

  void u1(uint8_t v) { bytes.push_back(v); }
  void u2(uint16_t v) {
    bytes.push_back((v >> 8) & 0xff);
    bytes.push_back(v & 0xff);
  }
  void u4(uint32_t v) {
    bytes.push_back((v >> 24) & 0xff);
    bytes.push_back((v >> 16) & 0xff);
    bytes.push_back((v >> 8) & 0xff);
    bytes.push_back(v & 0xff);
  }
  void utf8(const std::string& s) {
    u1(1); // CONSTANT_Utf8
    u2(s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
};

} // namespace

class JarLoaderTest : public RedexTest {};

// Regression test for JVM target 11 (JEP 280) bytecode: string concatenation is
// compiled to an `invokedynamic`, which adds a CONSTANT_InvokeDynamic (tag 18)
// constant-pool entry; condy adds CONSTANT_Dynamic (tag 17). The JarLoader used
// to abort on these tags. It only needs external symbols from the jar and never
// resolves invokedynamic bootstrap info, so it must parse past them.
TEST_F(JarLoaderTest, parsesInvokeDynamicAndDynamicConstants) {
  init_basic_types();

  ClassFileBuilder cf;
  cf.u4(0xcafebabe); // magic
  cf.u2(0); // minor version
  cf.u2(55); // major version (Java 11)

  // Constant pool (index 0 is reserved), 6 entries -> count = 7.
  cf.u2(7);
  cf.utf8("Foo"); // #1: this class name
  cf.u1(7); // #2: CONSTANT_Class
  cf.u2(1); //     name_index -> #1
  cf.utf8("java/lang/Object"); // #3: super class name
  cf.u1(7); // #4: CONSTANT_Class
  cf.u2(3); //     name_index -> #3
  cf.u1(18); // #5: CONSTANT_InvokeDynamic
  cf.u2(0); //     bootstrap_method_attr_index (unused by the loader)
  cf.u2(0); //     name_and_type_index (unused by the loader)
  cf.u1(17); // #6: CONSTANT_Dynamic
  cf.u2(0); //     bootstrap_method_attr_index (unused by the loader)
  cf.u2(0); //     name_and_type_index (unused by the loader)

  cf.u2(0x21); // access_flags: ACC_PUBLIC | ACC_SUPER
  cf.u2(2); // this_class -> #2 (Foo)
  cf.u2(4); // super_class -> #4 (java/lang/Object)
  cf.u2(0); // interfaces_count
  cf.u2(0); // fields_count
  cf.u2(0); // methods_count
  cf.u2(0); // attributes_count

  Scope classes;
  EXPECT_TRUE(parse_class(cf.bytes.data(),
                          cf.bytes.size(),
                          &classes,
                          /* attr_hook */ nullptr,
                          jar_loader::default_duplicate_allow_fn,
                          DexLocation::make_location("", "test.jar")));

  ASSERT_EQ(classes.size(), 1);
  EXPECT_STREQ(classes[0]->get_type()->get_name()->c_str(), "LFoo;");
}
