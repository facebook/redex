/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/S_Expression.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include <boost/functional/hash.hpp>

using namespace sparta;

namespace {

void check_s_expr_istream(s_expr_istream& input) { EXPECT_TRUE(input.good()); }

s_expr parse(const std::string& str) {
  std::istringstream str_input(str);
  s_expr_istream input(str_input);
  s_expr output;
  input >> output;
  // Using an EXPECT_* in a function that returns anything other than void
  // causes a compile-time error.
  check_s_expr_istream(input);
  return output;
}

void erroneous_parse(const std::string& str, size_t count, std::string& what) {
  std::istringstream str_input(str);
  s_expr_istream input(str_input);
  s_expr expr;
  while (count-- > 0) {
    input >> expr;
  }
  EXPECT_TRUE(input.fail());
  what = input.what();
}

} // namespace

TEST(S_ExpressionTest, basicOperations) {
  auto e1 = s_expr({s_expr("cons"), s_expr("a"),
                    s_expr({s_expr("cons"), s_expr("b"),
                            s_expr({s_expr("cons"), s_expr("c"), s_expr()})})});
  std::string e1_out = "(cons a (cons b (cons c ())))";
  EXPECT_EQ(e1_out, e1.str());
  EXPECT_EQ(e1, parse(e1_out));
  EXPECT_EQ(e1, e1);
  auto e1_2 = e1;
  EXPECT_EQ(e1, e1_2);
  EXPECT_FALSE(e1.is_atom());
  EXPECT_FALSE(e1.is_int32());
  EXPECT_FALSE(e1.is_string());
  EXPECT_TRUE(e1.is_list());
  EXPECT_FALSE(e1.is_nil());
  EXPECT_EQ(3, e1.size());
  EXPECT_EQ("cons", e1[0].get_string());
  EXPECT_EQ("a", e1[1].get_string());
  EXPECT_EQ(3, e1[2].size());
  EXPECT_EQ("cons", e1[2][0].get_string());
  EXPECT_EQ("b", e1[2][1].get_string());
  EXPECT_EQ(3, e1[2][2].size());
  EXPECT_EQ("cons", e1[2][2][0].get_string());
  EXPECT_EQ("c", e1[2][2][1].get_string());
  EXPECT_TRUE(e1[2][2][2].is_nil());
  EXPECT_EQ(0, e1[2][2][2].size());

  std::vector<s_expr> v2({s_expr(0), s_expr(-1),
                          s_expr(std::numeric_limits<int32_t>::min()),
                          s_expr(std::numeric_limits<int32_t>::max())});
  auto e2 = s_expr(v2);
  std::string e2_out;
  {
    std::ostringstream out;
    out << "(#0 #-1 #" << std::numeric_limits<int32_t>::min() << " #"
        << std::numeric_limits<int32_t>::max() << ")";
    e2_out = out.str();
  }
  EXPECT_EQ(e2_out, e2.str());
  EXPECT_EQ(e2, parse(e2_out));
  EXPECT_NE(e1, e2);

  {
    auto a1 = s_expr({s_expr("A"), s_expr({s_expr(-1), s_expr()})});
    auto a2 = s_expr({s_expr("A"), s_expr({s_expr(-1), s_expr()})});
    EXPECT_EQ(a1, a2);
  }

  std::vector<s_expr> v3({s_expr("abcd"), s_expr("a_b1"), s_expr("12345"),
                          s_expr("#abc{}()123!"),
                          s_expr("1ab\tcd\nef\"gh\"i")});
  auto e3 = s_expr(v3.begin(), v3.end());
  std::string e3_out =
      "(abcd a_b1 12345 \"#abc{}()123!\" \"1ab\tcd\nef\\\"gh\\\"i\")";
  EXPECT_EQ(e3_out, e3.str());
  EXPECT_EQ(e3, parse(e3_out));

  auto e4 = s_expr("123");
  std::string e4_out = "123";
  EXPECT_TRUE(e4.is_atom());
  EXPECT_FALSE(e4.is_int32());
  EXPECT_TRUE(e4.is_string());
  EXPECT_FALSE(e4.is_list());
  EXPECT_FALSE(e4.is_nil());
  EXPECT_EQ(e4, parse(e4_out));

  auto e5 = s_expr(123);
  std::string e5_out = "#123";
  EXPECT_TRUE(e5.is_atom());
  EXPECT_TRUE(e5.is_int32());
  EXPECT_FALSE(e5.is_string());
  EXPECT_FALSE(e5.is_list());
  EXPECT_FALSE(e5.is_nil());
  EXPECT_EQ(e5, parse(e5_out));

  std::unordered_set<s_expr, boost::hash<s_expr>> set1_5{e1, e2, e3, e4, e5};
  EXPECT_THAT(set1_5, ::testing::UnorderedElementsAre(e1, e2, e3, e4, e5));

  {
    std::ostringstream out;
    out << "\n\n    " << e2 << "\t\n\r \t\t" << e1 << "    " << e3 << e4
        << "\n\n\n\n"
        << e5;
    std::istringstream str_input(out.str());
    s_expr_istream input(str_input);
    s_expr i1, i2;
    input >> i1 >> i2;
    EXPECT_EQ(i1, e2);
    EXPECT_EQ(i2, e1);
    EXPECT_TRUE(input.good());
    std::vector<s_expr> exprs{i1, i2};
    for (;;) {
      s_expr e;
      input >> e;
      if (!input.good()) {
        break;
      }
      exprs.push_back(e);
    }
    EXPECT_TRUE(input.eoi());
    EXPECT_THAT(exprs, ::testing::ElementsAre(e2, e1, e3, e4, e5));
  }

  s_expr e6 = parse("(123#123()abc\"def\"\"gh()i\")");
  EXPECT_EQ("(123 #123 () abc def \"gh()i\")", e6.str());

  s_expr e7 = s_expr({s_expr("A"), s_expr("")});
  EXPECT_EQ("(A \"\")", e7.str());
  {
    std::istringstream str_input(e7.str());
    s_expr_istream input(str_input);
    s_expr i;
    input >> i;
    EXPECT_EQ(e7, i);
  }

  std::string error;
  erroneous_parse("((a) b ()", 1, error);
  EXPECT_EQ("On line 1: Incomplete S-expression", error);
  erroneous_parse("(\n(a)\nb\n()\n", 1, error);
  EXPECT_EQ("On line 5: Incomplete S-expression", error);
  erroneous_parse("((a) b c))", 2, error);
  EXPECT_EQ("On line 1: Extra ')' encountered", error);
  erroneous_parse(R"(
    (
      (a)
      b
      c
    ))
  )",
                  2,
                  error);
  EXPECT_EQ("On line 6: Extra ')' encountered", error);
  erroneous_parse("(a b #9999999999999)", 1, error);
  EXPECT_EQ("On line 1: Error parsing int32_t literal", error);
  erroneous_parse("(a b #-9999999999999)", 1, error);
  EXPECT_EQ("On line 1: Error parsing int32_t literal", error);
  erroneous_parse("(a b \"abcdef)", 1, error);
  EXPECT_EQ("On line 1: Error parsing string literal", error);
  erroneous_parse("123, (a b c)", 2, error);
  EXPECT_EQ("On line 1: Unexpected character encountered: ','", error);
  erroneous_parse(R"(;Should only take 1 endline in an inline comment\n\n\n
    (
      (const-string "foo\n\bar")
      123, (a b c)
    )
  )",
                  2,
                  error);
  EXPECT_EQ("On line 4: Unexpected character encountered: ','", error);
  erroneous_parse(R"(;The error should be on line 2
    (123, (a b c) ; End of line 2
  )",
                  2,
                  error);
  EXPECT_EQ("On line 2: Unexpected character encountered: ','", error);
}

TEST(S_ExpressionTest, patternMatching) {
  auto e1 = parse("((a #1) (b #2))");

  EXPECT_TRUE(s_patn({s_patn({s_patn("a"), s_patn(1)}),
                      s_patn({s_patn("b"), s_patn(2)})})
                  .match_with(e1));

  s_expr x, y, z;
  EXPECT_TRUE(
      s_patn({s_patn({s_patn("a"), s_patn(x)}), s_patn({s_patn("b")}, y)})
          .match_with(e1));
  EXPECT_TRUE(x.is_int32());
  EXPECT_EQ(1, x.get_int32());
  EXPECT_TRUE(y.is_list());
  EXPECT_EQ(s_expr({s_expr(2)}), y);

  std::string a, b;
  int32_t one, two;
  EXPECT_TRUE(s_patn({s_patn({s_patn(&a), s_patn(&one)}),
                      s_patn({s_patn(&b), s_patn(&two)})})
                  .match_with(e1));
  EXPECT_EQ("a", a);
  EXPECT_EQ("b", b);
  EXPECT_EQ(1, one);
  EXPECT_EQ(2, two);

  EXPECT_TRUE(
      s_patn({s_patn({s_patn(x), s_patn()}), s_patn({s_patn(y), s_patn()})})
          .match_with(e1));
  EXPECT_TRUE(x.is_string());
  EXPECT_EQ("a", x.get_string());
  EXPECT_TRUE(y.is_string());
  EXPECT_EQ("b", y.get_string());

  EXPECT_TRUE(
      s_patn({s_patn({s_patn(), s_patn(1)}), s_patn()}, x).match_with(e1));
  EXPECT_TRUE(x.is_nil());

  EXPECT_FALSE(s_patn({s_patn(x), s_patn(y), s_patn(z)}).match_with(e1));
  EXPECT_FALSE(s_patn({s_patn("a")}, y).match_with(e1));
  EXPECT_FALSE(
      s_patn({s_patn({s_patn("b"), s_patn(x)}), s_patn(y)}).match_with(e1));
  EXPECT_FALSE(
      s_patn({s_patn({s_patn("a"), s_patn(2)}), s_patn(y)}).match_with(e1));

  auto e2 = parse("(() (()))");
  EXPECT_TRUE(s_patn({s_patn({}), s_patn({s_patn({})})}).match_with(e2));

  auto e3 = parse("(a b () (c d) e)");
  EXPECT_TRUE(s_patn({s_patn("a"), s_patn(x), s_patn(y)}, z).match_with(e3));
  EXPECT_EQ(s_expr("b"), x);
  EXPECT_TRUE(y.is_nil());
  EXPECT_EQ(parse("((c d) e)"), z);
}
