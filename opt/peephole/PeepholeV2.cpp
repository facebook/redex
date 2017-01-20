/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PeepholeV2.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "Transform.h"
#include "Walkers.h"

////////////////////////////////////////////////////////////////////////////////
// PeepholeOptimizerV2 implementation
//
// Overview of the algorithm: Say we have the following code patterns to match
// and replace, and code sequence, where ; denotes basic block boundaries.
//
//           |  Match  | Replace |
// Pattern 1 |  a b c  |    x    |
// Pattern 2 |  a b d  |   y z   |
//
// Before:  ; a a b c a b d a f b d a b ; c a b d ;
//              ~~~~~ ~~~~~                 ~~~~~
// After:   ; a   x    y z  a f b d a b ; c  y z ;
//
// Assumptions:
//  (1) 'match' patterns do not span over multiple basic blocks as of now.
//      We may relax this restriction later.
//  (2) 'match' patterns cannot be interleaved by other instructions. In the
//      above example, "a f b d" won't be matched to "a b d" because of 'f'.
//      The current peephole implementation allows such interleaving as the
//      algorithm keeps track of data flow instead of pattern matching.
//
// This is essentially a string searching problem. We can ideally utilize
// std::search. But a full-fledged searching even with an optimal algorithm
// (e.g., Boyer-Moore) would take some time. ProGuard's approach is very
// interesting. Instead of a thorough searching, they applied a really simple
// heuristic when matching fails. For instance:
//
// Code:     a a b c     a a b c
//           | |           |
//           o x    ===>   o (retry)  ===> "a b c" will be matched
//           | |           |
// Pattern:  a b c         a b c   Only if matching fails on the second opcode
//                                 of the pattern, it retries to match the
//                                 current opcode and the pattern.
//
// Code:     a b a b c    a b a b c
//           | | |              |
//           o o x    ===>      x ..  ===> "a b c" won't be matched
//           | | |              |
// Pattern:  a b c              a  No retry. No rescan. Search resumes from the
//                                 the next opcode.
//
// So, on a matching failure, PG only retries when the failure occurs on the
// second opcode of the pattern. Otherwise, it simply moves forward. I would say
// this heuristic as a "sweeping" or "try-and-forget" algorithm because it only
// scans the code one time with very minimal retry. We first implement this PG's
// approach. (I don't know whether this is really intended or a bug.)
//
namespace {

struct DexPattern {
  const std::unordered_set<uint16_t> opcodes;
  const std::vector<uint16_t> srcs;
  const std::vector<uint16_t> dests;

  const enum class Kind {
    none,
    method,
    string,
    literal,
  } kind;

  DexMethod* const method;
  DexString* const string;
  int8_t const literal;
};

struct Pattern {
  const std::string name;
  const std::vector<DexPattern> match;
  const std::vector<DexPattern> replace;
};

// We need placeholders for 'pattern'. For instance, "const/4 kRegA, #B", has
// two placeholder indexes: (1) kRegA: a placeholder for register;
// (2) #B: a placeholder of literal.
//
// Register placeholders: it reserves even numbers for wide pairs.
static constexpr uint16_t kRegA = 0x01;
static constexpr uint16_t kRegB = 0x03;
static constexpr uint16_t kRegC = 0x05;
static constexpr uint16_t kRegD = 0x07;

// OPCODE_CONST_4 can only accept a 4-bit literal value while CONST_16/CONST
// allow larger numbers. Let's keep within 4-bit so that it can be used for all
// CONST-* family.
static constexpr int8_t kLiteralA = 0x1;
static constexpr int8_t kSingleByteCharA = 0x2;

// String placeholders for 'match' patterns. Returns a unique DexString
// pointers as an identifier.
DexString* string_A() { return g_redex->get_placeholder_string(0); }
DexString* string_B() { return g_redex->get_placeholder_string(1); }
DexString* string_empty() { return g_redex->get_placeholder_string(2); }

// We need special markers for 'replace' patterns. For instance, a special
// literal index to specify the length of 'string_A'.
//
// String placeholders for special operations:
// 'boolean_A_to_string' means "convert a boolean kLiteralA to string".
DexString* boolean_A_to_string() { return g_redex->get_placeholder_string(8); }
DexString* char_A_to_string() { return g_redex->get_placeholder_string(9); }
DexString* int_A_to_string() { return g_redex->get_placeholder_string(10); }
DexString* long_int_A_to_string() {
  return g_redex->get_placeholder_string(11);
}
DexString* float_A_to_string() { return g_redex->get_placeholder_string(12); }
DexString* double_A_to_string() { return g_redex->get_placeholder_string(13); }
DexString* concat_A_B_strings() { return g_redex->get_placeholder_string(14); }
DexString* concat_string_A_int_A() {
  return g_redex->get_placeholder_string(15);
}
DexString* concat_string_A_boolean_A() {
  return g_redex->get_placeholder_string(16);
}
DexString* concat_string_A_long_int_A() {
  return g_redex->get_placeholder_string(17);
}
DexString* concat_string_A_char_A() {
  return g_redex->get_placeholder_string(18);
}

// Compare string_A and string_B and write the result as a 4-bit integer.
static constexpr int8_t kLiteral4_StringCompare_A_B = 0x2;
// Get the length of string_A and write the result as a 16-bit integer.
static constexpr int8_t kLiteral16_StringLength_A = 0x3;

// Just a minimal refactor for long string constants.
static const char* LjavaString = "Ljava/lang/String;";
static const char* LjavaStringBuilder = "Ljava/lang/StringBuilder;";
static const char* LjavaObject = "Ljava/lang/Object;";

////////////////////////////////////////////////////////////////////////////////
// The patterns
const std::vector<Pattern>& get_patterns() {
  // Helpers
  //
  // invoke-direct {reg_instance}, Ljava/lang/StringBuilder;.<init>:()V
  auto invoke_StringBuilder_init = [](uint16_t reg_instance) -> DexPattern {
    return {{OPCODE_INVOKE_DIRECT},
            {reg_instance},
            {},
            DexPattern::Kind::method,
            DexMethod::make_method(LjavaStringBuilder, "<init>", "V", {}),
            nullptr,
            0};
  };

  // invoke-direct {reg_instance, reg_argument},
  // Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V
  auto invoke_StringBuilder_init_String = [](
      uint16_t reg_instance, uint16_t reg_argument) -> DexPattern {
    return {{OPCODE_INVOKE_DIRECT},
            {reg_instance, reg_argument},
            {},
            DexPattern::Kind::method,
            DexMethod::make_method(
                LjavaStringBuilder, "<init>", "V", {LjavaString}),
            nullptr,
            0};
  };

  // invoke-virtual {reg_instance, reg_argument},
  // Ljava/lang/StringBuilder;.append:(param_type)Ljava/lang/StringBuilder;
  auto invoke_StringBuilder_append = [](uint16_t reg_instance,
                                        uint16_t reg_argument,
                                        const char* param_type) -> DexPattern {
    std::vector<uint16_t> srcs;
    if (strcmp(param_type, "J") == 0 || strcmp(param_type, "D") == 0) {
      srcs = {reg_instance, reg_argument, uint16_t(reg_argument + 1)};
    } else {
      srcs = {reg_instance, reg_argument};
    }
    return {{OPCODE_INVOKE_VIRTUAL},
            std::move(srcs),
            {},
            DexPattern::Kind::method,
            DexMethod::make_method(
                LjavaStringBuilder, "append", LjavaStringBuilder, {param_type}),
            nullptr,
            0};
  };

  auto invoke_String_valueOf = [](uint16_t reg_argument,
                                  const char* param_type) -> DexPattern {
    std::vector<uint16_t> srcs;
    if (strcmp(param_type, "J") == 0 || strcmp(param_type, "D") == 0) {
      srcs = {reg_argument, uint16_t(reg_argument + 1)};
    } else {
      srcs = {reg_argument};
    }
    return {{OPCODE_INVOKE_STATIC},
            std::move(srcs),
            {},
            DexPattern::Kind::method,
            DexMethod::make_method(
                LjavaString, "valueOf", LjavaString, {param_type}),
            nullptr,
            0};
  };

  auto invoke_String_equals = [](uint16_t reg_instance,
                                 uint16_t reg_argument) -> DexPattern {
    return {{OPCODE_INVOKE_VIRTUAL},
            {reg_instance, reg_argument},
            {},
            DexPattern::Kind::method,
            DexMethod::make_method(LjavaString, "equals", "Z", {LjavaObject}),
            nullptr,
            0};
  };

  auto invoke_String_length = [](uint16_t reg_instance) -> DexPattern {
    return {{OPCODE_INVOKE_VIRTUAL},
            {reg_instance},
            {},
            DexPattern::Kind::method,
            DexMethod::make_method(LjavaString, "length", "I", {}),
            nullptr,
            0};
  };

  auto const_string = [](uint16_t dest, DexString* string) -> DexPattern {
    return {{OPCODE_CONST_STRING},
            {},
            {dest},
            DexPattern::Kind::string,
            nullptr,
            string,
            0};
  };

  auto move_result_object = [](uint16_t dest) -> DexPattern {
    return {{OPCODE_MOVE_RESULT_OBJECT},
            {},
            {dest},
            DexPattern::Kind::none,
            nullptr,
            nullptr,
            0};
  };

  auto move_result = [](uint16_t dest) -> DexPattern {
    return {{OPCODE_MOVE_RESULT},
            {},
            {dest},
            DexPattern::Kind::none,
            nullptr,
            nullptr,
            0};
  };

  auto const_literal = [](
      uint16_t opcode, uint16_t dest, int8_t literal) -> DexPattern {
    return {{opcode},
            {},
            {dest},
            DexPattern::Kind::literal,
            nullptr,
            nullptr,
            literal};
  };

  auto const_wide = [](uint16_t dest, int8_t literal) -> DexPattern {
    return {{OPCODE_CONST_WIDE_16, OPCODE_CONST_WIDE_32, OPCODE_CONST_WIDE},
            {},
            {dest},
            DexPattern::Kind::literal,
            nullptr,
            nullptr,
            literal};
  };

  auto const_integer = [](uint16_t dest, int8_t literal) -> DexPattern {
    return {{OPCODE_CONST_4, OPCODE_CONST_16, OPCODE_CONST},
            {},
            {dest},
            DexPattern::Kind::literal,
            nullptr,
            nullptr,
            literal};
  };

  auto const_float = [](uint16_t dest, int8_t literal) -> DexPattern {
    return {{OPCODE_CONST_4, OPCODE_CONST},
            {},
            {dest},
            DexPattern::Kind::literal,
            nullptr,
            nullptr,
            literal};
  };

  auto const_char = [](uint16_t dest, int8_t literal) -> DexPattern {
    // Modified UTF-8, 1-3 bytes. DX uses const/16 and const to load a char.
    return {{OPCODE_CONST_16, OPCODE_CONST},
            {},
            {dest},
            DexPattern::Kind::literal,
            nullptr,
            nullptr,
            literal};
  };

  static const std::vector<Pattern> kStringPatterns = {
      // It coalesces init(void) and append(string) into init(string).
      // new StringBuilder().append("...") = new StringBuilder("...")
      {"Coalesce_InitVoid_AppendString",
       {invoke_StringBuilder_init(kRegA),
        const_string(kRegB, string_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString),
        move_result_object(kRegA)},
       {// (3 + 2 + 3 + 1) - (2 + 3) = 4 code unit saving
        const_string(kRegB, string_A()),
        invoke_StringBuilder_init_String(kRegA, kRegB)}},

      // It coalesces consecutive two append(string) to a single append call.
      // StringBuilder.append("A").append("B") = StringBuilder.append("AB")
      {"Coalesce_AppendString_AppendString",
       {const_string(kRegB, string_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString),
        move_result_object(kRegC),
        const_string(kRegD, string_B()),
        invoke_StringBuilder_append(kRegC, kRegD, LjavaString)},
       {// 6 code unit saving
        const_string(kRegB, concat_A_B_strings()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString)}},

      // It evaluates the length of a literal in compile time.
      // "stringA".length() ==> length_of_stringA
      {"CompileTime_StringLength",
       {const_string(kRegA, string_A()),
        invoke_String_length(kRegA),
        move_result(kRegB)},
       {// 4 code unit saving
        const_literal(OPCODE_CONST_16, kRegB, kLiteral16_StringLength_A)}},

      // DISABLED: TODO: Found a crash, causing VerifyError
      // It removes an append call with an empty string.
      // StringBuilder.append("") = nothing
      // {"Remove_AppendEmptyString",
      //  {const_string(kRegB, string_empty()),
      //   invoke_StringBuilder_append(kRegA, kRegB, LjavaString)},
      //  {}},

      // TODO: It only handles a single-byte char.
      // It coalesces init(void) and append(char) into init(string).
      // StringBuilder().append(C) = new StringBuilder("....")
      {"Coalesce_Init_AppendChar",
       {invoke_StringBuilder_init(kRegA),
        const_char(kRegB, kSingleByteCharA),
        invoke_StringBuilder_append(kRegA, kRegB, "C"),
        move_result_object(kRegA)},
       {const_string(kRegB, char_A_to_string()),
        invoke_StringBuilder_init_String(kRegA, kRegB)}},

      // It coalesces append(string) and append(integer) into append(string).
      // StringBuilder.append("...").append(I) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendInt",
       {const_string(kRegB, string_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString),
        move_result_object(kRegC),
        const_integer(kRegD, kLiteralA),
        invoke_StringBuilder_append(kRegC, kRegD, "I")},
       {// (2 + 3 + 1 + [1, 3] + 3) - (2 + 3) = [5, 7] code unit saving
        const_string(kRegB, concat_string_A_int_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString)}},

      // TODO: It only handles a single-byte char.
      // It coalesces append(string) and append(char) into append(string).
      // StringBuilder.append("...").append(C) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendChar",
       {const_string(kRegB, string_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString),
        move_result_object(kRegC),
        const_char(kRegD, kSingleByteCharA),
        invoke_StringBuilder_append(kRegC, kRegD, "C")},
       {// (2 + 3 + 1 + 2 + 3) - (2 + 3) = 6 code unit saving
        const_string(kRegB, concat_string_A_char_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString)}},

      // It coalesces append(string) and append(boolean) into append(string).
      // StringBuilder.append("...").append(Z) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendBoolean",
       {const_string(kRegB, string_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString),
        move_result_object(kRegC),
        const_literal(OPCODE_CONST_4, kRegD, kLiteralA),
        invoke_StringBuilder_append(kRegC, kRegD, "Z")},
       {// (2 + 3 + 1 + 1 + 3) - (2 + 3) = 5 code unit saving
        const_string(kRegB, concat_string_A_boolean_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString)}},

      // It coalesces append(string) and append(long int) into append(string).
      // StringBuilder.append("...").append(J) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendLongInt",
       {const_string(kRegB, string_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString),
        move_result_object(kRegC),
        const_wide(kRegD, kLiteralA),
        invoke_StringBuilder_append(kRegC, kRegD, "J")},
       {// (2 + 3 + 1 + [2, 3, 5] + 3) - (2 + 3) = [6, 7, 9] code unit saving
        const_string(kRegB, concat_string_A_long_int_A()),
        invoke_StringBuilder_append(kRegA, kRegB, LjavaString)}},

      // It evaluates the identify of two literal strings in compile time.
      // "stringA".equals("stringB") ==> true or false
      {"CompileTime_StringCompare",
       {const_string(kRegA, string_A()),
        const_string(kRegB, string_B()),
        invoke_String_equals(kRegA, kRegB),
        move_result(kRegC)},
       {// (2 + 2 + 3 + 1) - 1 = 7 code unit saving
        const_literal(OPCODE_CONST_4, kRegC, kLiteral4_StringCompare_A_B)}},

      // It replaces valueOf on a boolean value by "true" or "false" directly.
      // String.valueof(true/false) ==> "true" or "false"
      {"Replace_ValueOfBoolean",
       {const_literal(OPCODE_CONST_4, kRegA, kLiteralA),
        invoke_String_valueOf(kRegA, "Z"),
        move_result_object(kRegB)},
       {// (1 + 3 + 1) - 2 = 3 16-bit code units saving
        const_string(kRegB, boolean_A_to_string())}},

      // TODO: It only handles a single-byte char.
      // It replaces valueOf on a literal character by the character itself.
      // String.valueOf(char) ==> "char"
      {"Replace_ValueOfChar",
       {const_char(kRegA, kSingleByteCharA),
        invoke_String_valueOf(kRegA, "C"),
        move_result_object(kRegB)},
       {// (2 + 3 + 1) - 2 = 4 units saving
        const_string(kRegB, char_A_to_string())}},

      // It replaces valueOf on an integer literal by the integer itself.
      // String.valueof(int) ==> "int"
      {"Replace_ValueOfInt",
       {const_integer(kRegA, kLiteralA),
        invoke_String_valueOf(kRegA, "I"),
        move_result_object(kRegB)},
       {// ([1, 2, 3] + 3 + 1) - 2 = [3, 4, 5] units saving
        const_string(kRegB, int_A_to_string())}},

      // It replaces valueOf on a long integer literal by the number itself.
      // String.valueof(long int) ==> "long int"
      {"Replace_ValueOfLongInt",
       {const_wide(kRegA, kLiteralA),
        invoke_String_valueOf(kRegA, "J"),
        move_result_object(kRegB)},
       {// ([2, 3, 5] + 3 + 1) - 2 = [4, 5, 7] units saving
        const_string(kRegB, long_int_A_to_string())}},

      // It replaces valueOf on a float literal by the float itself.
      // String.valueof(float) ==> "float"
      {"Replace_ValueOfFloat",
       {const_float(kRegA, kLiteralA),
        invoke_String_valueOf(kRegA, "F"),
        move_result_object(kRegB)},
       {// ([1, 3] + 3 + 1) - 2 = [3, 5] units saving
        const_string(kRegB, float_A_to_string())}},

      // It replaces valueOf on a double literal by the double itself.
      // String.valueof(double) ==> "double"
      {"Replace_ValueOfDouble",
       {const_wide(kRegA, kLiteralA),
        invoke_String_valueOf(kRegA, "D"),
        move_result_object(kRegB)},
       {// ([2, 3, 5] + 3 + 1) - 2 = [4, 5, 7] units saving
        const_string(kRegB, double_A_to_string())}},
  };
  return kStringPatterns;
}

template <typename Container, typename Key>
auto check_and_get(const Container& c, const Key& key) -> decltype(c.at(key)) {
  always_assert(c.find(key) != end(c));
  return c.at(key);
}

// Matcher holds the matching state for the given pattern.
struct Matcher {
  const Pattern& pattern;
  size_t match_index;
  std::vector<DexInstruction*> matched_instructions;
  std::unordered_map<uint16_t, uint16_t> matched_regs;
  std::unordered_map<DexString*, DexString*> matched_strings;
  std::unordered_map<int8_t /*literal identifier*/,
                     int64_t /*actual literal value*/>
      matched_literals;

  explicit Matcher(const Pattern& pattern) : pattern(pattern), match_index(0) {}

  void reset() {
    match_index = 0;
    matched_instructions.clear();
    matched_regs.clear();
    matched_strings.clear();
    matched_literals.clear();
  }

  // It updates the matching state for the given instruction. Returns true if
  // insn matches to the last 'match' pattern.
  bool try_match(DexInstruction* insn) {
    auto match_reg = [&](uint16_t pattern_reg, uint16_t insn_reg) {
      // This register has been observed already. Check whether they are same.
      if (matched_regs.find(pattern_reg) != end(matched_regs)) {
        return matched_regs.at(pattern_reg) == insn_reg;
      }
      // Newly observed. Remember it.
      matched_regs.emplace(pattern_reg, insn_reg);
      return true;
    };

    auto match_literal = [&](int8_t pattern_literal, int64_t insn_literal_val) {
      if (pattern_literal == kSingleByteCharA) {
        // A single-byte modified UTF-8 char is in the range of 0x01 to 0x7F.
        if (!(0x01 <= insn_literal_val && insn_literal_val <= 0x7F)) {
          return false;
        }
        // It is a single-byte char. Fall through.
      }
      if (matched_literals.find(pattern_literal) != end(matched_literals)) {
        return matched_literals.at(pattern_literal) == insn_literal_val;
      }
      matched_literals.emplace(pattern_literal, insn_literal_val);
      return true;
    };

    auto match_string = [&](DexString* pattern_str, DexString* insn_str) {
      if (pattern_str == string_empty()) {
        return (insn_str->is_simple() && insn_str->size() == 0);
      }
      if (matched_strings.find(pattern_str) != end(matched_strings)) {
        return matched_strings.at(pattern_str) == insn_str;
      }
      matched_strings.emplace(pattern_str, insn_str);
      return true;
    };

    // Does 'insn' match to the given DexPattern?
    auto match_instruction = [&](const DexPattern& pattern) {
      if (pattern.opcodes.find(insn->opcode()) == end(pattern.opcodes) ||
          pattern.srcs.size() != insn->srcs_size() ||
          pattern.dests.size() != insn->dests_size()) {
        return false;
      }

      if (pattern.dests.size() != 0) {
        assert(pattern.dests.size() == 1);
        if (!match_reg(pattern.dests[0], insn->dest())) {
          return false;
        }
      }

      for (size_t i = 0; i < pattern.srcs.size(); ++i) {
        if (!match_reg(pattern.srcs[i], insn->src(i))) {
          return false;
        }
      }

      switch (pattern.kind) {
      case DexPattern::Kind::none:
        return true;
      case DexPattern::Kind::string:
        return match_string(
            pattern.string,
            static_cast<const DexOpcodeString*>(insn)->get_string());
      case DexPattern::Kind::literal:
        return match_literal(pattern.literal, insn->literal());
      case DexPattern::Kind::method:
        return pattern.method ==
               static_cast<const DexOpcodeMethod*>(insn)->get_method();
      }
      return false;
    };

    assert(match_index < pattern.match.size());
    if (!match_instruction(pattern.match[match_index])) {
      // Okay, this is the PG's heuristic. Retry only if the failure occurs on
      // the second opcode of the pattern.
      bool retry = (match_index == 1);
      TRACE(PEEPHOLE,
            8,
            "Not Matched: %s[%lu] != %s\n",
            pattern.name.c_str(),
            match_index,
            SHOW(insn));
      reset();
      if (retry) {
        assert(match_index == 0);
        if (!match_instruction(pattern.match[match_index])) {
          return false;
        }
      } else {
        return false;
      }
    }

    TRACE(PEEPHOLE,
          8,
          "Matched [%lu/%lu]: %s\n",
          match_index + 1,
          pattern.match.size(),
          SHOW(insn));
    matched_instructions.push_back(insn);
    ++match_index;
    return match_index == pattern.match.size();
  }

  DexInstruction* generate_replacement(const DexPattern& replace) {
    if (replace.opcodes.size() != 1) {
      always_assert_log(false, "Replacement must have unique opcode");
      return nullptr;
    }

    const auto opcode = *begin(replace.opcodes);
    switch (opcode) {
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_VIRTUAL:
      assert(replace.kind == DexPattern::Kind::method);
      return (new DexOpcodeMethod(opcode, replace.method))
          ->set_arg_word_count(replace.srcs.size())
          ->set_srcs(replace.srcs);

    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT:
      assert(replace.kind == DexPattern::Kind::none);
      assert(replace.dests.size() == 1);
      return (new DexInstruction(opcode))->set_dest(replace.dests[0]);

    case OPCODE_CONST_STRING:
      assert(replace.kind == DexPattern::Kind::string);
      assert(replace.dests.size() == 1);
      return (new DexOpcodeString(OPCODE_CONST_STRING, replace.string))
          ->set_dest(replace.dests[0]);

    case OPCODE_CONST_4:
    case OPCODE_CONST_16:
    case OPCODE_CONST:
      assert(replace.kind == DexPattern::Kind::literal);
      assert(replace.dests.size() == 1);
      return (new DexInstruction(opcode))
          ->set_dest(replace.dests[0])
          ->set_literal(replace.literal);
    }

    always_assert_log(false, "Unhandled opcode: 0x%x", opcode);
    return nullptr;
  }

  // After a successful match, get the replacement instructions. We substitute
  // the placeholders appropriately including special command placeholders.
  std::list<DexInstruction*> get_replacements() {
    always_assert(pattern.match.size() == match_index);

    std::list<DexInstruction*> replace;
    for (const auto& r : pattern.replace) {
      // First, generate the instruction with placeholders.
      auto clone = generate_replacement(r);
      replace.push_back(clone);

      // Patch the placeholders with actual arguments.
      if (clone->dests_size() > 0) {
        const auto reg = clone->dest();
        always_assert(matched_regs.find(reg) != end(matched_regs));
        clone->set_dest(matched_regs.at(reg));
      }

      for (unsigned i = 0; i < clone->srcs_size(); ++i) {
        const auto reg = clone->src(i);
        always_assert(matched_regs.find(reg) != end(matched_regs));
        clone->set_src(i, matched_regs.at(reg));
      }

      if (clone->has_literal()) {
        const auto literal = clone->literal();
        if (literal == kLiteral4_StringCompare_A_B) {
          auto a = check_and_get(matched_strings, string_A());
          auto b = check_and_get(matched_strings, string_B());
          clone->set_literal((a == b) ? 1L : 0L);
        } else if (literal == kLiteral16_StringLength_A) {
          auto a = check_and_get(matched_strings, string_A());
          clone->set_literal(a->length());
        } else if (literal == kLiteralA) {
          auto a = check_and_get(matched_literals, kLiteralA);
          clone->set_literal(a);
        } else {
          always_assert_log(false, "Unexpected literal 0x%x", literal);
        }
      }

      if (clone->has_strings()) {
        const auto& str = static_cast<DexOpcodeString*>(clone)->get_string();
        if (str == boolean_A_to_string()) {
          bool a = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(a == true ? "true" : "false"));
        } else if (str == char_A_to_string()) {
          // TODO: FIXME: currently it only handles a single-byte char.
          // A modified UTF-8 character can be 1-3 bytes.
          int a = check_and_get(matched_literals, kSingleByteCharA);
          assert(0x01 <= a && a <= 0x7F);
          // As 'a' is a single-byte character, treat it as an ASCII char.
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::string(1, a)));
        } else if (str == int_A_to_string()) {
          int a = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::to_string(a)));
        } else if (str == long_int_A_to_string()) {
          int64_t a = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::to_string(a)));
        } else if (str == float_A_to_string()) {
          union {
            int32_t i;
            float f;
          } a = {.i = static_cast<int32_t>(
                     check_and_get(matched_literals, kLiteralA))};
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::to_string(a.f)));
        } else if (str == double_A_to_string()) {
          union {
            int64_t i;
            double d;
          } a = {.i = check_and_get(matched_literals, kLiteralA)};
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::to_string(a.d)));
        } else if (str == string_A()) {
          auto a = check_and_get(matched_strings, string_A());
          static_cast<DexOpcodeString*>(clone)->rewrite_string(a);
        } else if (str == concat_A_B_strings()) {
          auto a = check_and_get(matched_strings, string_A())->c_str();
          auto b = check_and_get(matched_strings, string_B())->c_str();
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::string(a) + std::string(b)));
        } else if (str == concat_string_A_int_A()) {
          auto a = check_and_get(matched_strings, string_A())->c_str();
          int b = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::string(a) + std::to_string(b)));
        } else if (str == concat_string_A_boolean_A()) {
          auto a = check_and_get(matched_strings, string_A())->c_str();
          bool b = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::string(a) +
                                     (b == true ? "true" : "false")));
        } else if (str == concat_string_A_long_int_A()) {
          auto a = check_and_get(matched_strings, string_A())->c_str();
          int64_t b = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::string(a) + std::to_string(b)));
        } else if (str == concat_string_A_char_A()) {
          // TODO: FIXME: currently it only handles a single-byte char.
          auto a = check_and_get(matched_strings, string_A())->c_str();
          int b = check_and_get(matched_literals, kSingleByteCharA);
          assert(0x01 <= b && b <= 0x7F);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::string(a) + std::string(1, b)));
        } else {
          always_assert_log(false, "Unexpected string");
        }
      }
    }
    return replace;
  }
};

class PeepholeOptimizerV2 {
 private:
  const std::vector<DexClass*>& m_scope;
  std::vector<Matcher> m_matchers;
  std::vector<int> m_matchers_stat;
  int m_stats_removed = 0;
  int m_stats_inserted = 0;

 public:
  explicit PeepholeOptimizerV2(const std::vector<DexClass*>& scope)
      : m_scope(scope) {
    for (const auto& pattern : get_patterns()) {
      m_matchers.emplace_back(pattern);
      m_matchers_stat.push_back(0);
    }
  }

  void peephole(DexMethod* method) {
    auto transform =
        MethodTransform::get_method_transform(method, true /* want_cfg */);

    std::vector<DexInstruction*> deletes;
    std::vector<std::pair<DexInstruction*, std::list<DexInstruction*>>> inserts;
    const auto& blocks = transform->cfg();
    for (const auto& block : blocks) {
      // Currently, all patterns do not span over multiple basic blocks. So
      // reset all matching states on visiting every basic block.
      for (auto& m : m_matchers) {
        m.reset();
      }

      for (auto& mei : *block) {
        if (mei.type != MFLOW_OPCODE) {
          continue;
        }

        for (size_t i = 0; i < m_matchers.size(); ++i) {
          auto& matcher = m_matchers[i];
          if (!matcher.try_match(mei.insn)) {
            continue;
          }

          assert(i < m_matchers.size());
          ++m_matchers_stat[i];
          TRACE(PEEPHOLE, 8, "PATTERN MATCHED!\n");
          deletes.insert(end(deletes),
                         begin(matcher.matched_instructions),
                         end(matcher.matched_instructions));

          auto replace = matcher.get_replacements();
          for (const auto& r : replace) {
            TRACE(PEEPHOLE, 8, "-- %s\n", SHOW(r));
          }

          m_stats_inserted += replace.size();
          m_stats_removed += matcher.match_index;

          inserts.emplace_back(mei.insn, replace);
          matcher.reset();
          break; // Matched. No need to check other patterns.
        }
      }
    }

    for (auto& pair : inserts) {
      transform->insert_after(pair.first, pair.second);
    }
    for (auto& insn : deletes) {
      transform->remove_opcode(insn);
    }
  }

  void print_stats() {
    TRACE(PEEPHOLE, 1, "%d instructions removed\n", m_stats_removed);
    TRACE(PEEPHOLE, 1, "%d instructions inserted\n", m_stats_inserted);
    TRACE(PEEPHOLE,
          1,
          "%d net instruction change\n",
          m_stats_inserted - m_stats_removed);
    TRACE(PEEPHOLE,
          1,
          "%lu patterns matched and replaced\n",
          std::accumulate(begin(m_matchers_stat), end(m_matchers_stat), 0L));
    TRACE(PEEPHOLE, 5, "Detailed pattern match stats:\n");
    for (size_t i = 0; i < m_matchers_stat.size(); ++i) {
      TRACE(PEEPHOLE,
            5,
            "%s: %d\n",
            m_matchers[i].pattern.name.c_str(),
            m_matchers_stat[i]);
    }
  }

  void run() {
    walk_methods(m_scope, [&](DexMethod* m) {
      if (m->get_code()) {
        peephole(m);
      }
    });

    print_stats();
  }
};
}

void PeepholePassV2::run_pass(DexStoresVector& stores,
                              ConfigFiles& /*cfg*/,
                              PassManager& /*mgr*/) {
  auto scope = build_class_scope(stores);
  PeepholeOptimizerV2(scope).run();
}

static PeepholePassV2 s_pass;
