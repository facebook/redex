/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "SourceDebugExtension.h"

using source_debug_extension::SourceDebugExtension;

namespace {

constexpr std::string_view kSourceMap = R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
+ 1 Inline.kt
src/Inline.kt
2 Caller.kt
*L
10#1,3:100,2
*S KotlinDebug
*F
1 Caller.kt
*L
42#1:102
*E
)";

} // namespace

TEST(SourceDebugExtensionTest, MapsDefaultStratumAndKotlinCallsite) {
  auto extension = SourceDebugExtension::parse(kSourceMap);
  ASSERT_TRUE(extension);

  auto first = extension->map(100);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->source.file, "Inline.kt");
  EXPECT_EQ(first->source.line, 10);
  EXPECT_TRUE(first->callers.empty());

  auto repeated = extension->map(102);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->source.file, "Inline.kt");
  EXPECT_EQ(repeated->source.line, 11);
  ASSERT_EQ(repeated->callers.size(), 1);
  EXPECT_EQ(repeated->callers[0].file, "Caller.kt");
  EXPECT_EQ(repeated->callers[0].line, 42);

  EXPECT_FALSE(extension->map(106));
}

TEST(SourceDebugExtensionTest, MapsNestedKotlinInlineCallers) {
  auto extension = SourceDebugExtension::parse(R"(SMAP
InlineTestCodeKt.kt
Kotlin
*S Kotlin
*F
1 InlineTestCodeKt.kt
2 KotlinInlineFunctions.kt
*L
1#1,37:1
12#2:38
22#2,2:39
27#2:41
22#2,7:42
*S KotlinDebug
*F
1 InlineTestCodeKt.kt
*L
21#1:38
27#1:39,2
33#1:41
33#1:42,7
*E
)");
  ASSERT_TRUE(extension);

  auto separate = extension->map(39);
  ASSERT_TRUE(separate);
  EXPECT_EQ(separate->source.file, "KotlinInlineFunctions.kt");
  EXPECT_EQ(separate->source.line, 22);
  ASSERT_EQ(separate->callers.size(), 1);
  EXPECT_EQ(separate->callers[0].file, "InlineTestCodeKt.kt");
  EXPECT_EQ(separate->callers[0].line, 27);

  auto outer = extension->map(41);
  ASSERT_TRUE(outer);
  EXPECT_EQ(outer->source.file, "KotlinInlineFunctions.kt");
  EXPECT_EQ(outer->source.line, 27);
  ASSERT_EQ(outer->callers.size(), 1);
  EXPECT_EQ(outer->callers[0].file, "InlineTestCodeKt.kt");
  EXPECT_EQ(outer->callers[0].line, 33);

  auto inner = extension->map(42);
  ASSERT_TRUE(inner);
  EXPECT_EQ(inner->source.file, "KotlinInlineFunctions.kt");
  EXPECT_EQ(inner->source.line, 22);
  ASSERT_EQ(inner->callers.size(), 2);
  EXPECT_EQ(inner->callers[0].file, "KotlinInlineFunctions.kt");
  EXPECT_EQ(inner->callers[0].line, 27);
  EXPECT_EQ(inner->callers[1].file, "InlineTestCodeKt.kt");
  EXPECT_EQ(inner->callers[1].line, 33);
}

TEST(SourceDebugExtensionTest, SupportsImplicitFileIds) {
  auto extension = SourceDebugExtension::parse(R"(SMAP
Generated.jsp
JSP
*S	JSP
*F
2 Generated.jsp

1 Other.jsp
*L

5:20
6,2:21
*E  
)");
  ASSERT_TRUE(extension);
  auto first = extension->map(20);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->source.file, "Generated.jsp");
  EXPECT_EQ(first->source.line, 5);
  auto repeated = extension->map(22);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->source.line, 7);
}

TEST(SourceDebugExtensionTest, RejectsMalformedMaps) {
  EXPECT_FALSE(SourceDebugExtension::parse("not a source map"));
  EXPECT_FALSE(SourceDebugExtension::parse(R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
1 Inline.kt
*L
10#2:100
*E
)"));
  EXPECT_FALSE(SourceDebugExtension::parse(R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
1 Inline.kt
*L
4294967295#1,2:100
*E
)"));
  EXPECT_FALSE(SourceDebugExtension::parse(R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
1 Inline.kt
*L
10#1:100,0
*E
)"));
  EXPECT_FALSE(SourceDebugExtension::parse(R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
1 Inline.kt
*L
10#1,2:100
20#1:101
*E
)"));
}
