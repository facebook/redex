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

struct Pattern {
  std::vector<const DexInstruction*> match;
  std::vector<const DexInstruction*> replace;
};

// We need placeholders for 'match' patterns. For instance, "const/4 kRegA, #B",
// has two placeholder indexes: (1) kRegA: a placeholder for register; (2) B:
// a placeholder of literal.
//
static constexpr uint16_t kRegA = 0x01;
static constexpr uint16_t kRegB = 0x02;
static constexpr uint16_t kRegC = 0x03;
static constexpr uint16_t kRegD = 0x03;

static constexpr int64_t kLiteralA = 0x01;

// String placeholders for 'match' patterns. Returns a unique DexString
// pointers. They are constant memory leaks. It's ugly.
DexString* string_A() { return g_redex->get_placeholder_string(0); }

DexString* string_B() { return g_redex->get_placeholder_string(1); }

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

// Compare string_A and string_B and write the result as a 4-bit integer.
static constexpr int64_t kLiteral4_StringCompare_A_B = 0x01;
// Get the length of string_A and write the result as a 16-bit integer.
static constexpr int64_t kLiteral16_StringLength_A = 0x02;

////////////////////////////////////////////////////////////////////////////////
// The patterns
// - Refer to InstructionSequenceConstants::STRING in ProGuard.
const std::vector<Pattern>& get_patterns() {
  // Just an alias
  DexMethod* (&M)(
      const char*, const char*, const char*, std::vector<const char*>) =
      DexMethod::make_method;

  // TODO: We could implement "dpattern" like "dasm".
  static const std::vector<Pattern> kPatterns = {
      {{// Pattern 1: this checks to see if two literal strings are identical.
        // "stringA".equals("stringB") ==> true or false
        (new DexOpcodeString(OPCODE_CONST_STRING, string_A()))->set_dest(kRegA),
        (new DexOpcodeString(OPCODE_CONST_STRING, string_B()))->set_dest(kRegB),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_VIRTUAL,
             M("Ljava/lang/String;", "equals", "Z", {"Ljava/lang/Object;"}),
             0))
            ->set_arg_word_count(2)
            ->set_src(0, kRegA)
            ->set_src(1, kRegB),
        (new DexInstruction(OPCODE_MOVE_RESULT))->set_dest(kRegC)},
       {// Compare string A and B and put the constant.
        (new DexInstruction(OPCODE_CONST_4))
            ->set_dest(kRegC)
            ->set_literal(kLiteral4_StringCompare_A_B)}},

      {{// Pattern 2: this compile time evaluates the length of a literal
        // string.
        // "stringA".length() ==> length_of_stringA
        (new DexOpcodeString(OPCODE_CONST_STRING, string_A()))->set_dest(kRegA),
        (new DexOpcodeMethod(OPCODE_INVOKE_VIRTUAL,
                             M("Ljava/lang/String;", "length", "I", {}),
                             0))
            ->set_arg_word_count(1)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT))->set_dest(kRegB)},
       {// length_of_stringA
        (new DexInstruction(OPCODE_CONST_16))
            ->set_dest(kRegB)
            ->set_literal(kLiteral16_StringLength_A)}},

      {{// Pattern 3: this compile time evaluates true/false for valueof calls.
        // String.valueof(true/false) ==> "true" or "false"
        (new DexInstruction(OPCODE_CONST_4))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"Z"}),
             0))
            ->set_arg_word_count(1)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(boolean)
        (new DexOpcodeString(OPCODE_CONST_STRING, boolean_A_to_string()))
            ->set_dest(kRegB)}},

      {{// Pattern 4: calling valueof on a literal character is the character
        // itself.
        // String.valueof(char) ==> "char"
        (new DexInstruction(OPCODE_CONST_16))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"C"}),
             0))
            ->set_arg_word_count(1)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(char)
        (new DexOpcodeString(OPCODE_CONST_STRING, char_A_to_string()))
            ->set_dest(kRegB)}},

      {{// Pattern 5: calling valueof on a literal number destined for a short
        // container can be replaced by the short literal.
        // String.valueof(short) ==> "short"
        (new DexInstruction(OPCODE_CONST_16))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"I"}),
             0))
            ->set_arg_word_count(1)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(int)
        (new DexOpcodeString(OPCODE_CONST_STRING, int_A_to_string()))
            ->set_dest(kRegB)}},

      {{// Pattern 6: likewise, but for int values
        // String.valueof(int) ==> "int"
        (new DexInstruction(OPCODE_CONST))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"I"}),
             0))
            ->set_arg_word_count(1)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(int)
        (new DexOpcodeString(OPCODE_CONST_STRING, int_A_to_string()))
            ->set_dest(kRegB)}},

      {{// Pattern 7: likewise, but for long int values
        // String.valueof(long int) ==> "long int"
        (new DexInstruction(OPCODE_CONST_WIDE))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"J"}),
             0))
            ->set_arg_word_count(2)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(long int)
        (new DexOpcodeString(OPCODE_CONST_STRING, long_int_A_to_string()))
            ->set_dest(kRegB)}},

      {{// Pattern 8: likewise, but for float values
        // String.valueof(float) ==> "float"
        (new DexInstruction(OPCODE_CONST))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"F"}),
             0))
            ->set_arg_word_count(1)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(float)
        (new DexOpcodeString(OPCODE_CONST_STRING, float_A_to_string()))
            ->set_dest(kRegB)}},

      {{// Pattern 9: likewise, but for double values
        // String.valueof(double) ==> "double"
        (new DexInstruction(OPCODE_CONST_WIDE))
            ->set_dest(kRegA)
            ->set_literal(kLiteralA),
        (new DexOpcodeMethod(
             OPCODE_INVOKE_STATIC,
             M("Ljava/lang/String;", "valueOf", "Ljava/lang/String;", {"D"}),
             0))
            ->set_arg_word_count(2)
            ->set_src(0, kRegA),
        (new DexInstruction(OPCODE_MOVE_RESULT_OBJECT))->set_dest(kRegB)},
       {// to_string(double)
        (new DexOpcodeString(OPCODE_CONST_STRING, double_A_to_string()))
            ->set_dest(kRegB)}},

  };
  return kPatterns;
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
  std::unordered_map<const void*, const void*> matched_args;
  std::unordered_map<int64_t, int64_t> matched_literals;

  explicit Matcher(const Pattern& pattern) : pattern(pattern), match_index(0) {}

  void reset() {
    match_index = 0;
    matched_instructions.clear();
    matched_regs.clear();
    matched_args.clear();
    matched_literals.clear();
  }

  // match updates the matching state for the given instruction. Returns true if
  // insn matches to the last 'match' pattern.
  bool match(DexInstruction* insn) {
    auto match_reg = [&](uint16_t pattern_reg, uint16_t insn_reg) {
      // This register has been observed already. Check whether they are same.
      if (matched_regs.find(pattern_reg) != end(matched_regs)) {
        return matched_regs.at(pattern_reg) == insn_reg;
      }
      // Newly observed. Remember it.
      matched_regs[pattern_reg] = insn_reg;
      return true;
    };

    auto match_literal = [&](int64_t pattern_literal, int64_t insn_literal) {
      if (matched_literals.find(pattern_literal) != end(matched_literals)) {
        return matched_literals.at(pattern_literal) == insn_literal;
      }
      matched_literals[pattern_literal] = insn_literal;
      return true;
    };

    // Check additional arguments like strings and fields.
    auto match_argument = [&](const void* pattern_arg, const void* insn_arg) {
      if (matched_args.find(pattern_arg) != end(matched_args)) {
        return matched_args.at(pattern_arg) == insn_arg;
      }
      matched_args[pattern_arg] = insn_arg;
      return true;
    };

    // Does 'insn' match to pattern.match[match_index]?
    // Note: the current implementation returns false for specific cases and
    // returns true for the rest. More safe algorithm would be the opposite:
    // return true only for certain cases; otherwise false.
    auto match_instruction = [&](const DexInstruction* check) {
      if (check->opcode() != insn->opcode() ||
          check->dests_size() != insn->dests_size() ||
          check->srcs_size() != insn->srcs_size()) {
        return false;
      }

      if (check->dests_size()) {
        if (!match_reg(check->dest(), insn->dest())) {
          return false;
        }
      }

      for (unsigned i = 0; i < check->srcs_size(); ++i) {
        if (!match_reg(check->src(i), insn->src(i))) {
          return false;
        }
      }

      if (check->has_strings()) {
        if (!match_argument(
                static_cast<const DexOpcodeString*>(check)->get_string(),
                static_cast<const DexOpcodeString*>(insn)->get_string())) {
          return false;
        }
      }

      if (check->has_literal()) {
        if (!match_literal(check->literal(), insn->literal())) {
          return false;
        }
      }

      if (check->has_methods()) {
        if (static_cast<const DexOpcodeMethod*>(check)->get_method() !=
            static_cast<const DexOpcodeMethod*>(insn)->get_method()) {
          return false;
        };
      }

      if (check->has_fields() || check->has_offset()) {
        return false;
      }

      return true;
    };

    assert(match_index < pattern.match.size());
    if (!match_instruction(pattern.match[match_index])) {
      // Okay, this is the PG's heuristic. Retry only if the failure occurs on
      // the second opcode of the pattern.
      bool retry = (match_index == 1);
      TRACE(PEEPHOLE,
            8,
            "Not Matched: %s != %s\n",
            SHOW(pattern.match[match_index]->opcode()),
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
    // TODO: just reset match_index here if it returns true to prevent an out of
    // bound error.
    return match_index == pattern.match.size();
  }

  // After a successful match, get the replacement instructions. We have to
  // substitute the arguments appropriately, also need to handle special
  // command placeholders.
  std::list<DexInstruction*> get_replacements() {
    always_assert(pattern.match.size() == match_index);

    std::list<DexInstruction*> replace;
    for (const auto& r : pattern.replace) {
      // Clone first, then patch arguments.
      auto clone = r->clone();
      replace.push_back(clone);

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
          const auto& a = check_and_get(matched_args, string_A());
          const auto& b = check_and_get(matched_args, string_B());
          clone->set_literal((a == b) ? 1L : 0L);
        } else if (literal == kLiteral16_StringLength_A) {
          const auto& a = check_and_get(matched_args, string_A());
          clone->set_literal(static_cast<const DexString*>(a)->size());
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
          char a = check_and_get(matched_literals, kLiteralA);
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
          auto a = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::to_string(*((float*)&a))));
        } else if (str == double_A_to_string()) {
          auto a = check_and_get(matched_literals, kLiteralA);
          static_cast<DexOpcodeString*>(clone)->rewrite_string(
              DexString::make_string(std::to_string(*((double*)&a))));
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
          if (!matcher.match(mei.insn)) {
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
            "[%lu/%lu]: %d\n",
            i + 1,
            m_matchers_stat.size(),
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
