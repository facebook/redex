/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Peephole.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "RedundantCheckCastRemover.h"
#include "Walkers.h"

////////////////////////////////////////////////////////////////////////////////
// PeepholeOptimizer implementation
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

// The peephole first detects code patterns like "const-string v0, "foo"".
// We need identifiers to describe the arguments of each instruction such as
// registers, method, literals, etc. For instance, we need an identifier for
// an arbitrary literal argument. We may need an identifier only for an empty
// string.
//
// Once a pattern is detected, the original instructions are replaced by new
// instructions. Sometimes we need to patch the arguments of the new
// instructions. For instance, we want to write the length of string A.
// We also need a special identifier for this action.
enum class Register : uint16_t {
  // It reserves only even numbers for wide pairs.
  A = 1,
  B = 3,
  C = 5,
  D = 7,
  E = 9,

  pair_A = 2,
  pair_B = 4,
  pair_C = 6,
  pair_D = 8,
};

enum class Literal {
  // For an arbitrary literal argument
  A,
  // Directive: Compare strings A and B and write the result as a 4-bit integer.
  Compare_Strings_A_B,
  // Directive: Write the length of string A as a 16-bit integer.
  Length_String_A,
  // Directive: Convert mul/div to shl/shr with log2 of the literal argument.
  Mul_Div_To_Shift_Log2,
};

enum class String {
  // For arbitrary string arguments
  A,
  B,
  // For only an empty string argument
  empty,

  // Special string argument directives for replacements
  boolean_A_to_string, // e.g., convert literal A as a boolean to a string.
  char_A_to_string,
  int_A_to_string,
  long_int_A_to_string,
  float_A_to_string,
  double_A_to_string,
  concat_A_B_strings,
  concat_string_A_boolean_A,
  concat_string_A_char_A,
  concat_string_A_int_A,
  concat_string_A_long_int_A,
  Type_A_get_simple_name,
};

enum class Type {
  A,
  B,
};

enum class Field {
  A,
  B,
};

// Just a minimal refactor for long string constants.
static const char* LjavaString = "Ljava/lang/String;";
static const char* LjavaStringBuilder = "Ljava/lang/StringBuilder;";
static const char* LjavaObject = "Ljava/lang/Object;";

struct DexPattern {
  const std::unordered_set<uint16_t> opcodes;
  const std::vector<Register> srcs;
  const std::vector<Register> dests;

  const enum class Kind {
    none,
    method,
    string,
    literal,
    type,
    copy, // Replace with the same exact instruction we matched. No change.
    field,
  } kind;

  const union {
    std::nullptr_t const dummy;
    DexMethodRef* const method;
    String const string;
    Literal const literal;
    Type const type;
    unsigned int const copy_index;
    Field field;
  };

  DexPattern(std::unordered_set<uint16_t>&& opcodes,
             std::vector<Register>&& srcs,
             std::vector<Register>&& dests)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::none),
        dummy(nullptr) {}

  DexPattern(std::unordered_set<uint16_t>&& opcodes,
             std::vector<Register>&& srcs,
             std::vector<Register>&& dests,
             DexMethodRef* const method)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::method),
        method(method) {}

  DexPattern(std::unordered_set<uint16_t>&& opcodes,
             std::vector<Register>&& srcs,
             std::vector<Register>&& dests,
             const String string)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::string),
        string(string) {}

  DexPattern(std::unordered_set<uint16_t>&& opcodes,
             std::vector<Register>&& srcs,
             std::vector<Register>&& dests,
             const Literal literal)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::literal),
        literal(literal) {}

  DexPattern(std::unordered_set<uint16_t>&& opcodes,
             std::vector<Register>&& srcs,
             std::vector<Register>&& dests,
             const Type type)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::type),
        type(type) {}

  DexPattern(std::unordered_set<uint16_t>&& opcodes,
             std::vector<Register>&& srcs,
             std::vector<Register>&& dests,
             const Field field)
      : opcodes(std::move(opcodes)),
        srcs(std::move(srcs)),
        dests(std::move(dests)),
        kind(DexPattern::Kind::field),
        field(field) {}

  static const DexPattern copy_matched_instruction(int index) {
    return DexPattern(index);
  }

 private:
  explicit DexPattern(unsigned int index)
      : kind(Kind::copy), copy_index(index) {}
};

struct Matcher;

struct Pattern {
  const std::string name;
  const std::vector<DexPattern> match;
  const std::vector<DexPattern> replace;
  const std::function<bool(const Matcher&)> predicate;

  Pattern(std::string name,
          std::vector<DexPattern> match,
          std::vector<DexPattern> replace,
          std::function<bool(const Matcher&)> predicate = {})
      : name(std::move(name)),
        match(std::move(match)),
        replace(std::move(replace)),
        predicate(std::move(predicate)) {}
};

// Matcher holds the matching state for the given pattern.
struct Matcher {
  const Pattern& pattern;
  size_t match_index;
  std::vector<IRInstruction*> matched_instructions;

  std::unordered_map<Register, uint16_t, EnumClassHash> matched_regs;
  std::unordered_map<String, DexString*, EnumClassHash> matched_strings;
  std::unordered_map<Literal, int64_t, EnumClassHash> matched_literals;
  std::unordered_map<Type, DexType*, EnumClassHash> matched_types;
  std::unordered_map<Field, DexFieldRef*, EnumClassHash> matched_fields;

  explicit Matcher(const Pattern& pattern) : pattern(pattern), match_index(0) {}

  void reset() {
    match_index = 0;
    matched_instructions.clear();
    matched_regs.clear();
    matched_strings.clear();
    matched_literals.clear();
    matched_types.clear();
    matched_fields.clear();
  }

  // It updates the matching state for the given instruction. Returns true if
  // insn matches to the last 'match' pattern.
  bool try_match(IRInstruction* insn) {
    auto match_reg = [&](Register pattern_reg, uint16_t insn_reg) {
      // This register has been observed already. Check whether they are same.
      if (matched_regs.find(pattern_reg) != end(matched_regs)) {
        return matched_regs.at(pattern_reg) == insn_reg;
      }

      // Newly observed. Remember it.
      matched_regs.emplace(pattern_reg, insn_reg);
      return true;
    };

    auto match_literal = [&](Literal lit_pattern, int64_t insn_literal_val) {
      if (matched_literals.find(lit_pattern) != end(matched_literals)) {
        return matched_literals.at(lit_pattern) == insn_literal_val;
      }
      matched_literals.emplace(lit_pattern, insn_literal_val);
      return true;
    };

    auto match_string = [&](String str_pattern, DexString* insn_str) {
      if (str_pattern == String::empty) {
        return (insn_str->is_simple() && insn_str->size() == 0);
      }
      if (matched_strings.find(str_pattern) != end(matched_strings)) {
        return matched_strings.at(str_pattern) == insn_str;
      }
      matched_strings.emplace(str_pattern, insn_str);
      return true;
    };

    auto match_type = [&](Type type_pattern, DexType* insn_type) {
      if (matched_types.find(type_pattern) != end(matched_types)) {
        return matched_types.at(type_pattern) == insn_type;
      }
      matched_types.emplace(type_pattern, insn_type);
      return true;
    };

    auto match_field = [&](Field field_pattern, DexFieldRef* insn_field) {
      auto result = matched_fields.emplace(field_pattern, insn_field);
      bool newly_inserted = result.second;
      return newly_inserted ? true : result.first->second == insn_field;
    };

    // Does 'insn' match to the given DexPattern?
    auto match_instruction = [&](const DexPattern& dex_pattern) {
      if (dex_pattern.opcodes.find(insn->opcode()) ==
              end(dex_pattern.opcodes) ||
          dex_pattern.srcs.size() != insn->srcs_size() ||
          dex_pattern.dests.size() != insn->dests_size()) {
        return false;
      }

      if (dex_pattern.dests.size() != 0) {
        redex_assert(dex_pattern.dests.size() == 1);
        if (!match_reg(dex_pattern.dests[0], insn->dest())) {
          return false;
        }
      }

      for (size_t i = 0; i < dex_pattern.srcs.size(); ++i) {
        if (!match_reg(dex_pattern.srcs[i], insn->src(i))) {
          return false;
        }
      }
      switch (dex_pattern.kind) {
      case DexPattern::Kind::none:
        return true;
      case DexPattern::Kind::string:
        return match_string(dex_pattern.string, insn->get_string());
      case DexPattern::Kind::literal:
        return match_literal(dex_pattern.literal, insn->get_literal());
      case DexPattern::Kind::method:
        return dex_pattern.method == insn->get_method();
      case DexPattern::Kind::type:
        return match_type(dex_pattern.type, insn->get_type());
      case DexPattern::Kind::field:
        return match_field(dex_pattern.field, insn->get_field());
      case DexPattern::Kind::copy:
        always_assert_log(
            false, "Kind::copy can only be used in replacements. Not matches");
      }
      return false;
    };

    redex_assert(match_index < pattern.match.size());
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
        redex_assert(match_index == 0);
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

    bool done = match_index == pattern.match.size();

    // if we've matched everything, the predicate may still veto
    if (done && pattern.predicate && !pattern.predicate(*this)) {
      reset();
      return false;
    }
    return done;
  }

  // Generate skeleton instruction for the replacement.
  IRInstruction* generate_dex_instruction(const DexPattern& replace) {
    if (replace.opcodes.size() != 1) {
      always_assert_log(false, "Replacement must have unique opcode");
      return nullptr;
    }

    const auto opcode = *begin(replace.opcodes);
    switch (opcode) {
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_VIRTUAL:
      redex_assert(replace.kind == DexPattern::Kind::method);
      return (new IRInstruction((IROpcode)opcode))
          ->set_method(replace.method)
          ->set_arg_word_count(replace.srcs.size());

    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE:
    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT:
    case IOPCODE_MOVE_RESULT_PSEUDO:
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_NEG_INT:
      redex_assert(replace.kind == DexPattern::Kind::none);
      return new IRInstruction((IROpcode)opcode);

    case OPCODE_CONST_STRING:
      redex_assert(replace.kind == DexPattern::Kind::string);
      return new IRInstruction(OPCODE_CONST_STRING);

    case OPCODE_CONST:
    case OPCODE_SHR_INT_LIT8:
    case OPCODE_SHL_INT_LIT8:
      redex_assert(replace.kind == DexPattern::Kind::literal);
      return new IRInstruction((IROpcode)opcode);

    case OPCODE_IPUT:
    case OPCODE_IPUT_BYTE:
    case OPCODE_IPUT_CHAR:
    case OPCODE_IPUT_BOOLEAN:
    case OPCODE_IPUT_SHORT:
    case OPCODE_IPUT_WIDE:
    case OPCODE_IPUT_OBJECT:
    case OPCODE_IGET:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IGET_SHORT:
    case OPCODE_IGET_WIDE:
    case OPCODE_IGET_OBJECT:
    case OPCODE_SPUT:
    case OPCODE_SPUT_BYTE:
    case OPCODE_SPUT_CHAR:
    case OPCODE_SPUT_BOOLEAN:
    case OPCODE_SPUT_SHORT:
    case OPCODE_SPUT_WIDE:
    case OPCODE_SPUT_OBJECT:
    case OPCODE_SGET:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_SHORT:
    case OPCODE_SGET_WIDE:
    case OPCODE_SGET_OBJECT:
      redex_assert(replace.kind == DexPattern::Kind::field);
      return new IRInstruction(static_cast<IROpcode>(opcode));

    case OPCODE_APUT:
    case OPCODE_APUT_BYTE:
    case OPCODE_APUT_CHAR:
    case OPCODE_APUT_BOOLEAN:
    case OPCODE_APUT_SHORT:
    case OPCODE_APUT_WIDE:
    case OPCODE_APUT_OBJECT:
    case OPCODE_AGET:
    case OPCODE_AGET_BYTE:
    case OPCODE_AGET_CHAR:
    case OPCODE_AGET_BOOLEAN:
    case OPCODE_AGET_SHORT:
    case OPCODE_AGET_WIDE:
    case OPCODE_AGET_OBJECT:
      redex_assert(replace.kind == DexPattern::Kind::none);
      return new IRInstruction(static_cast<IROpcode>(opcode));
    }

    always_assert_log(false, "Unhandled opcode: 0x%x", opcode);
    return nullptr;
  }

  // After a successful match, get the replacement instructions. We substitute
  // the placeholders appropriately including special command placeholders.
  std::vector<IRInstruction*> get_replacements() {
    always_assert(pattern.match.size() == match_index);

    std::vector<IRInstruction*> replacements;
    for (const auto& replace_info : pattern.replace) {
      // First, generate the instruction object.
      if (replace_info.kind == DexPattern::Kind::copy) {
        always_assert(matched_instructions.size() > replace_info.copy_index);
        replacements.push_back(
            new IRInstruction(*matched_instructions[replace_info.copy_index]));
        continue;
      }
      auto replace = generate_dex_instruction(replace_info);
      replacements.push_back(replace);

      // Fill the arguments appropriately.
      if (replace_info.dests.size() > 0) {
        redex_assert(replace_info.dests.size() == 1);
        const Register dest = replace_info.dests[0];
        always_assert(matched_regs.find(dest) != end(matched_regs));
        replace->set_dest(matched_regs.at(dest));
      }

      for (size_t i = 0; i < replace_info.srcs.size(); ++i) {
        const Register reg = replace_info.srcs[i];
        always_assert(matched_regs.find(reg) != end(matched_regs));
        replace->set_src(i, matched_regs.at(reg));
      }

      if (replace_info.kind == DexPattern::Kind::string) {
        switch (replace_info.string) {
        case String::A: {
          auto a = matched_strings.at(String::A);
          replace->set_string(a);
          break;
        }
        case String::B: {
          auto b = matched_strings.at(String::B);
          replace->set_string(b);
          break;
        }
        case String::empty: {
          auto empty = DexString::make_string("");
          replace->set_string(empty);
          break;
        }
        case String::boolean_A_to_string: {
          bool a = matched_literals.at(Literal::A);
          replace->set_string(
              DexString::make_string(a == true ? "true" : "false"));
          break;
        }
        case String::char_A_to_string: {
          int a = matched_literals.at(Literal::A);
          auto achar = encode_utf8_char_to_mutf8_string(a);
          replace->set_string(DexString::make_string(achar.c_str(), 1));
          break;
        }
        case String::int_A_to_string: {
          int a = matched_literals.at(Literal::A);
          replace->set_string(DexString::make_string(std::to_string(a)));
          break;
        }
        case String::long_int_A_to_string: {
          int64_t a = matched_literals.at(Literal::A);
          replace->set_string(DexString::make_string(std::to_string(a)));
          break;
        }
        case String::float_A_to_string: {
          union {
            int32_t i;
            float f;
          } a;
          a.i = static_cast<int32_t>(matched_literals.at(Literal::A));
          replace->set_string(DexString::make_string(std::to_string(a.f)));
          break;
        }
        case String::double_A_to_string: {
          union {
            int64_t i;
            double d;
          } a;
          a.i = matched_literals.at(Literal::A);
          replace->set_string(DexString::make_string(std::to_string(a.d)));
          break;
        }
        case String::concat_A_B_strings: {
          auto a = matched_strings.at(String::A)->c_str();
          auto b = matched_strings.at(String::B)->c_str();
          replace->set_string(
              DexString::make_string(std::string(a) + std::string(b)));
          break;
        }
        case String::concat_string_A_int_A: {
          auto a = matched_strings.at(String::A)->c_str();
          int b = matched_literals.at(Literal::A);
          replace->set_string(
              DexString::make_string(std::string(a) + std::to_string(b)));
          break;
        }
        case String::concat_string_A_boolean_A: {
          auto a = matched_strings.at(String::A)->c_str();
          bool b = matched_literals.at(Literal::A);
          replace->set_string(DexString::make_string(
              std::string(a) + (b == true ? "true" : "false")));
          break;
        }
        case String::concat_string_A_long_int_A: {
          auto a = matched_strings.at(String::A)->c_str();
          int64_t b = matched_literals.at(Literal::A);
          replace->set_string(
              DexString::make_string(std::string(a) + std::to_string(b)));
          break;
        }
        case String::concat_string_A_char_A: {
          auto a = matched_strings.at(String::A)->c_str();
          int b = matched_literals.at(Literal::A);
          auto bchar = encode_utf8_char_to_mutf8_string(b);
          replace->set_string(DexString::make_string(std::string(a) + bchar));
          break;
        }
        case String::Type_A_get_simple_name: {
          DexType* a = matched_types.at(Type::A);
          std::string simple = get_simple_name(a);
          replace->set_string(DexString::make_string(simple));
          break;
        }
        default:
          always_assert_log(
              false, "Unexpected string directive: 0x%x", replace_info.string);
          break;
        }
      } else if (replace_info.kind == DexPattern::Kind::literal) {
        switch (replace_info.literal) {
        case Literal::Compare_Strings_A_B: {
          auto a = matched_strings.at(String::A);
          auto b = matched_strings.at(String::B);
          // Just DexString* pointer comparison! DexString has uniqueness.
          replace->set_literal((a == b) ? 1L : 0L);
          break;
        }
        case Literal::Length_String_A: {
          auto a = matched_strings.at(String::A);
          replace->set_literal(a->length());
          break;
        }
        case Literal::A: {
          auto a = matched_literals.at(Literal::A);
          replace->set_literal(a);
          break;
        }
        case Literal::Mul_Div_To_Shift_Log2: {
          auto a = matched_literals.at(Literal::Mul_Div_To_Shift_Log2);
          redex_assert(a > 0);
          replace->set_literal(static_cast<uint64_t>(log2(a)));
          break;
        }
        default:
          always_assert_log(
              false, "Unexpected literal directive 0x%x", replace_info.literal);
          break;
        }
      } else if (replace_info.kind == DexPattern::Kind::type) {
        switch (replace_info.type) {
        case Type::A:
          replace->set_type(matched_types.at(Type::A));
          break;
        case Type::B:
          replace->set_type(matched_types.at(Type::B));
          break;
        default:
          always_assert_log(
              false, "Unexpected type directive 0x%x", replace_info.type);
          break;
        }
      } else if (replace_info.kind == DexPattern::Kind::field) {
        switch (replace_info.field) {
        case Field::A:
          replace->set_field(matched_fields.at(Field::A));
          break;
        case Field::B:
          replace->set_field(matched_fields.at(Field::B));
        default:
          always_assert_log(
              false, "Unexpected field directive 0x%x", replace_info.field);
        }
      }
    }
    return replacements;
  }
};

// The optimization MUST NOT change the state of the registers after the viewed
// piece of code runs. Changing the registers is unsafe because some later
// instruction may depend on that register and the peephole has no clue. So, it
// must be conservative. This means that the peephole optimization will create
// dead writes that Dead Code Elimination (DCE) will clean up later.
//
// Another constraint on register state:
// When restoring register state, you MUST do so in the same order as before the
// optimization. The reason is that multiple symbolic registers (like
// Register::A and Register::B) can map to the same real register (like v1).
// An example:
//
// const A, 0   matches  const v1, 0
// const B, 1            const v1, 1
//
// If you were to change the order, v1 would have the wrong value.
//
// Individual patterns can be disabled via config
// "PeepholePass" : {
//    "disabled_peepholes" : [
//      "Name_OfOpt1",
//      "etc."
//    ]
// }
namespace patterns {

// invoke-direct {reg_instance}, Ljava/lang/StringBuilder;.<init>:()V
DexPattern invoke_StringBuilder_init(Register instance) {
  return {{OPCODE_INVOKE_DIRECT},
          {instance},
          {},
          DexMethod::make_method(LjavaStringBuilder, "<init>", "V", {})};
}

// invoke-direct {reg_instance, reg_argument},
// Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V
DexPattern invoke_StringBuilder_init_String(Register instance,
                                            Register argument) {
  return {
      {OPCODE_INVOKE_DIRECT},
      {instance, argument},
      {},
      DexMethod::make_method(LjavaStringBuilder, "<init>", "V", {LjavaString})};
};

// invoke-virtual {reg_instance, reg_argument},
// Ljava/lang/StringBuilder;.append:(param_type)Ljava/lang/StringBuilder;
DexPattern invoke_StringBuilder_append(Register instance,
                                       Register argument,
                                       const char* param_type) {
  return {{OPCODE_INVOKE_VIRTUAL},
          {instance, argument},
          {},
          DexMethod::make_method(
              LjavaStringBuilder, "append", LjavaStringBuilder, {param_type})};
};

DexPattern invoke_String_valueOf(Register argument, const char* param_type) {
  return {{OPCODE_INVOKE_STATIC},
          {argument},
          {},
          DexMethod::make_method(
              LjavaString, "valueOf", LjavaString, {param_type})};
};

DexPattern invoke_String_equals(Register instance, Register argument) {
  return {{OPCODE_INVOKE_VIRTUAL},
          {instance, argument},
          {},
          DexMethod::make_method(LjavaString, "equals", "Z", {LjavaObject})};
};

DexPattern invoke_String_length(Register instance) {
  return {{OPCODE_INVOKE_VIRTUAL},
          {instance},
          {},
          DexMethod::make_method(LjavaString, "length", "I", {})};
};

DexPattern const_string(String string) {
  return {{OPCODE_CONST_STRING}, {}, {}, string};
};

DexPattern move_result_pseudo_wide(Register dest) {
  return {{IOPCODE_MOVE_RESULT_PSEUDO_WIDE}, {}, {dest}};
};

DexPattern move_result_pseudo(Register dest) {
  return {{IOPCODE_MOVE_RESULT_PSEUDO}, {}, {dest}};
};

DexPattern move_result_pseudo_object(Register dest) {
  return {{IOPCODE_MOVE_RESULT_PSEUDO_OBJECT}, {}, {dest}};
};

DexPattern move_result_object(Register dest) {
  return {{OPCODE_MOVE_RESULT_OBJECT}, {}, {dest}};
};

DexPattern move_result(Register dest) {
  return {{OPCODE_MOVE_RESULT}, {}, {dest}};
};

DexPattern const_literal(uint16_t opcode, Register dest, Literal literal) {
  return {{opcode}, {}, {dest}, literal};
};

DexPattern const_wide(Register dest, Literal literal) {
  return {{OPCODE_CONST_WIDE},
          {},
          {dest},
          literal};
};

DexPattern const_integer(Register dest, Literal literal) {
  return {{OPCODE_CONST}, {}, {dest}, literal};
};

DexPattern const_float(Register dest, Literal literal) {
  return {{OPCODE_CONST}, {}, {dest}, literal};
};

DexPattern const_char(Register dest, Literal literal) {
  // Modified UTF-8, 1-3 bytes. DX uses const/4 for the null character
  // (\u0000), and const/16 and const to load a char.
  return const_integer(dest, literal);
};

DexPattern move_object(Register dest, Register src) {
  return {{OPCODE_MOVE_OBJECT}, {src}, {dest}};
};

static const std::vector<Pattern>& get_string_patterns() {
  static const std::vector<Pattern> kStringPatterns = {
      // It coalesces init(void) and append(string) into init(string).
      // new StringBuilder().append("...") = new StringBuilder("...")
      {"Coalesce_InitVoid_AppendString",
       {invoke_StringBuilder_init(Register::A),
        const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::A)},
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_init_String(Register::A, Register::B)}},

      // It coalesces consecutive two append(string) to a single append call.
      // StringBuilder.append("A").append("B") = StringBuilder.append("AB")
      {"Coalesce_AppendString_AppendString",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_string(String::B),
        move_result_pseudo_object(Register::D),
        invoke_StringBuilder_append(Register::C, Register::D, LjavaString),
        move_result_object(Register::E)},
       // pre opt write order: B, C, D, E
       {const_string(String::concat_A_B_strings),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        const_string(String::B), // maybe dead
        move_result_pseudo_object(Register::D),
        move_object(Register::E, Register::C)}}, // maybe dead
      // post opt write order B, B, C, D, E

      // Explanation of WithoutMoveResult
      // A variation of the above optimization. The result of append isn't
      // always moved with move-result-object. But we want to capture both forms
      // of this pattern. This optimization would not be safe if
      // AppendString_AppendString doesn't run first because
      // (1) the last instruction of the pattern is an invoke AND
      // (2) the last instruction of the replacement is not an invoke AND
      // (3) the instruction after the pattern may be a move_result_object
      {"Coalesce_AppendString_AppendString_WithoutMoveResult",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_string(String::B),
        move_result_pseudo_object(Register::D),
        invoke_StringBuilder_append(Register::C, Register::D, LjavaString)},
       // pre opt write order: B, C, D
       {const_string(String::concat_A_B_strings),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        const_string(String::B), // maybe dead
        move_result_pseudo_object(Register::D)}},
      // there shouldn't be a move-result-object here because of the
      // previous pattern
      // post opt write order: B, B, C, D

      // It evaluates the length of a literal in compile time.
      // "stringA".length() ==> length_of_stringA
      {"CompileTime_StringLength",
       {const_string(String::A),
        move_result_pseudo_object(Register::A),
        invoke_String_length(Register::A),
        move_result(Register::B)},
       {const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::A),
        const_literal(OPCODE_CONST, Register::B, Literal::Length_String_A)}},

      // It removes an append call with an empty string.
      // StringBuilder.append("") = nothing
      {"Remove_AppendEmptyString",
       {const_string(String::empty),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C)},
       {const_string(String::empty), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A)}}, // maybe dead

      {"Remove_AppendEmptyString_WithoutMoveResult",
       {const_string(String::empty),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString)},
       {const_string(String::empty),
        move_result_pseudo_object(Register::B)}}, // maybe dead

      // It coalesces init(void) and append(char) into init(string).
      // StringBuilder().append(C) = new StringBuilder("....")
      {"Coalesce_Init_AppendChar",
       {invoke_StringBuilder_init(Register::A),
        const_char(Register::B, Literal::A),
        invoke_StringBuilder_append(Register::A, Register::B, "C"),
        move_result_object(Register::C)},
       {const_string(String::char_A_to_string),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_init_String(Register::A, Register::B),
        DexPattern::copy_matched_instruction(1), // const_char. maybe dead
        move_object(Register::C, Register::A)}}, // maybe dead

      {"Coalesce_Init_AppendChar_WithoutMoveResult",
       {invoke_StringBuilder_init(Register::A),
        const_char(Register::B, Literal::A),
        invoke_StringBuilder_append(Register::A, Register::B, "C")},
       {const_string(String::char_A_to_string),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_init_String(Register::A, Register::B),
        DexPattern::copy_matched_instruction(1)}}, // const_char. maybe dead

      // It coalesces append(string) and append(integer) into append(string).
      // StringBuilder.append("...").append(I) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendInt",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_integer(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "I"),
        move_result_object(Register::E)},
       // pre opt write order: B, C, D, E
       {// (2 + 3 + 1 + [1, 2, 3] + 3 + 1) - (2 + 3 + 2 + 1 + [1, 2, 3] + 1) = 1
        const_string(String::concat_string_A_int_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        DexPattern::copy_matched_instruction(4), // const_integer. maybe dead
        move_object(Register::E, Register::C)}}, // maybe dead
      // post opt write order B, B, C, D, E

      {"Coalesce_AppendString_AppendInt_WithoutMoveResult",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_integer(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "I")},
       // pre opt write order: B, C, D
       {// (2 + 3 + 1 + [1, 2, 3] + 3) - (2 + 3 + 2 + 1 + [1, 2, 3]) = 1
        const_string(String::concat_string_A_int_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        DexPattern::copy_matched_instruction(4)}}, // const_integer. maybe dead
      // post opt write order: B, B, C, D

      // It coalesces append(string) and append(char) into append(string).
      // StringBuilder.append("...").append(C) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendChar",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_char(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "C"),
        move_result_object(Register::E)},
       // pre opt write order: B, C, D, A
       {// (2 + 3 + 1 + [1, 2, 3] + 3 + 1) - (2 + 3 + 2 + 1 + [1, 2, 3] + 1) = 1
        const_string(String::concat_string_A_char_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        DexPattern::copy_matched_instruction(4), // const_integer. maybe dead
        move_object(Register::E, Register::C)}}, // maybe dead
      // post opt write order: B, B, C, D, E

      {"Coalesce_AppendString_AppendChar_WithoutMoveResult",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_char(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "C")},
       // pre opt write order: B, C, D
       {// (2 + 3 + 1 + [1, 2, 3] + 3) - (2 + 3 + 2 + 1 + [1, 2, 3]) = 1
        const_string(String::concat_string_A_char_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        DexPattern::copy_matched_instruction(4)}}, // const_integer. maybe dead
      // post opt write order: B, B, C, D

      // It coalesces append(string) and append(boolean) into append(string).
      // StringBuilder.append("...").append(Z) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendBoolean",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_literal(OPCODE_CONST, Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "Z"),
        move_result_object(Register::E)},
       // pre opt write order: B, C, D, E
       {const_string(String::concat_string_A_boolean_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        const_literal(OPCODE_CONST, Register::D, Literal::A), // maybe dead
        move_object(Register::E, Register::C)}}, // maybe dead
      // post opt write order: B, B, C, D, E

      {"Coalesce_AppendString_AppendBoolean_WithoutMoveResult",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_literal(OPCODE_CONST, Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "Z")},
       // pre opt write order: B, C, D
       {const_string(String::concat_string_A_boolean_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        const_literal(OPCODE_CONST, Register::D, Literal::A)}}, // maybe dead
      // post opt write order: B, B, C, D

      // It coalesces append(string) and append(long int) into append(string).
      // StringBuilder.append("...").append(J) = StringBuilder.append("....")
      {"Coalesce_AppendString_AppendLongInt",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_wide(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "J"),
        move_result_object(Register::E)},
       // pre opt write order: B, C, D, E
       {// (2 + 3 + 1 + [2, 3, 5] + 3 + 1) - (2 + 3 + 2 + 1 + [2, 3, 5] + 1) = 1
        const_string(String::concat_string_A_long_int_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        DexPattern::copy_matched_instruction(4), // const_wide. maybe dead
        move_object(Register::E, Register::C)}}, // maybe dead
      // post opt write order: B, B, C, D, E

      {"Coalesce_AppendString_AppendLongInt_WithoutMoveResult",
       {const_string(String::A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        move_result_object(Register::C),
        const_wide(Register::D, Literal::A),
        invoke_StringBuilder_append(Register::C, Register::D, "J")},
       // pre opt write order: B, C, D
       {// (2 + 3 + 1 + [2, 3, 5] + 3) - (2 + 3 + 2 + 1 + [2, 3, 5]) = 1
        const_string(String::concat_string_A_long_int_A),
        move_result_pseudo_object(Register::B),
        invoke_StringBuilder_append(Register::A, Register::B, LjavaString),
        const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::B),
        move_object(Register::C, Register::A), // maybe dead
        DexPattern::copy_matched_instruction(4)}}, // const_wide. maybe dead
      // post opt write order: B, B, C, D

      // It evaluates the identify of two literal strings in compile time.
      // "stringA".equals("stringB") ==> true or false
      {"CompileTime_StringCompare",
       {const_string(String::A),
        move_result_pseudo_object(Register::A),
        const_string(String::B),
        move_result_pseudo_object(Register::B),
        invoke_String_equals(Register::A, Register::B),
        move_result(Register::C)},
       {const_string(String::A), // maybe dead
        move_result_pseudo_object(Register::A),
        const_string(String::B), // maybe dead
        move_result_pseudo_object(Register::B),
        const_literal(
            OPCODE_CONST, Register::C, Literal::Compare_Strings_A_B)}},

      // It replaces valueOf on a boolean value by "true" or "false" directly.
      // String.valueof(true/false) ==> "true" or "false"
      {"Replace_ValueOfBoolean",
       {const_literal(OPCODE_CONST, Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "Z"),
        move_result_object(Register::B)},
       {const_literal(OPCODE_CONST, Register::A, Literal::A), // maybe dead
        const_string(String::boolean_A_to_string),
        move_result_pseudo_object(Register::B)}},

      // It replaces valueOf on a literal character by the character itself.
      // String.valueOf(char) ==> "char"
      {"Replace_ValueOfChar",
       {const_char(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "C"),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_char. maybe dead
        const_string(String::char_A_to_string), // maybe dead
        move_result_pseudo_object(Register::B)}},

      // It replaces valueOf on an integer literal by the integer itself.
      // String.valueof(int) ==> "int"
      {"Replace_ValueOfInt",
       {const_integer(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "I"),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_integer. maybe dead
        const_string(String::int_A_to_string),
        move_result_pseudo_object(Register::B)}},

      // It replaces valueOf on a long integer literal by the number itself.
      // String.valueof(long int) ==> "long int"
      {"Replace_ValueOfLongInt",
       {const_wide(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "J"),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_wide. maybe dead
        const_string(String::long_int_A_to_string),
        move_result_pseudo_object(Register::B)}},

      // It replaces valueOf on a float literal by the float itself.
      // String.valueof(float) ==> "float"
      {"Replace_ValueOfFloat",
       {const_float(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "F"),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_float. maybe dead
        const_string(String::float_A_to_string),
        move_result_pseudo_object(Register::B)}},

      // It replaces valueOf on a double literal by the double itself.
      // String.valueof(double) ==> "double"
      {"Replace_ValueOfDouble",
       {const_wide(Register::A, Literal::A),
        invoke_String_valueOf(Register::A, "D"),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_wide. maybe dead
        const_string(String::double_A_to_string),
        move_result_pseudo_object(Register::B)}},
  };
  return kStringPatterns;
}

DexPattern move_ops(Register dest, Register src) {
  return {{OPCODE_MOVE, OPCODE_MOVE_OBJECT}, {src}, {dest}};
};

const std::vector<Pattern>& get_nop_patterns() {
  static const std::vector<Pattern> kNopPatterns = {
      // Remove redundant move and move_object instructions,
      // e.g. move v0, v0
      {"Remove_Redundant_Move", {move_ops(Register::A, Register::A)}, {}},
  };
  return kNopPatterns;
}

static bool second_get_non_volatile(const Matcher& m) {
  if (m.matched_instructions.size() < 2) {
    return false;
  }

  DexFieldRef* field_ref = m.matched_instructions[1]->get_field();
  if (!field_ref->is_concrete()) {
    return false;
  }

  DexField* field = static_cast<DexField*>(field_ref);
  return !(field->get_access() & ACC_VOLATILE);
}

DexPattern put_x_op(IROpcode opcode,
                    Register src,
                    Register obj_register,
                    Field field) {
  if (is_iput(opcode)) {
    return {{opcode}, {src, obj_register}, {}, field};
  }
  if (is_sput(opcode)) {
    return {{opcode}, {src}, {}, field};
  }
  always_assert_log(false, "Not supported IROpcode %s", SHOW(opcode));
}

DexPattern get_x_op(IROpcode opcode, Register src, Field field) {
  if (is_iget(opcode)) {
    return {{opcode}, {src}, {}, field};
  }
  if (is_sget(opcode)) {
    return {{opcode}, {}, {}, field};
  }
  always_assert_log(false, "Not supported IROpcode %s", SHOW(opcode));
}

std::vector<DexPattern> put_x_patterns(IROpcode put_code) {
  return {put_x_op(put_code, Register::A, Register::B, Field::A)};
}

std::vector<DexPattern> put_get_x_patterns(
    IROpcode put_code,
    IROpcode get_code,
    DexPattern (*move_pseudo_func)(Register reg)) {
  return {put_x_op(put_code, Register::A, Register::B, Field::A),
          get_x_op(get_code, Register::B, Field::A),
          move_pseudo_func(Register::A)};
}

DexPattern aput_x_op(IROpcode opcode,
                     Register src,
                     Register array_register,
                     Register index_register) {
  if (is_aput(opcode)) {
    return {{opcode}, {src, array_register, index_register}, {}};
  }
  always_assert_log(false, "Not supported IROpcode %s", SHOW(opcode));
}

std::vector<DexPattern> aput_x_patterns(IROpcode put_code) {
  return {aput_x_op(put_code, Register::A, Register::B, Register::C)};
}

DexPattern aget_x_op(IROpcode opcode,
                     Register array_register,
                     Register index_register) {
  if (is_aget(opcode)) {
    return {{opcode}, {array_register, index_register}, {}};
  }
  always_assert_log(false, "Not supported IROpcode %s", SHOW(opcode));
}

std::vector<DexPattern> aput_aget_x_patterns(
    IROpcode aput_code,
    IROpcode aget_code,
    DexPattern (*move_pseudo_func)(Register reg)) {
  return {aput_x_op(aput_code, Register::A, Register::B, Register::C),
          aget_x_op(aget_code, Register::B, Register::C),
          move_pseudo_func(Register::A)};
}

const std::vector<Pattern>& get_aputaget_patterns() {
  static const auto* kAputAgetPatterns = new std::vector<Pattern>(
      {{"Replace_AputAget",
        aput_aget_x_patterns(OPCODE_APUT, OPCODE_AGET, move_result_pseudo),
        aput_x_patterns(OPCODE_APUT)},
       {"Replace_AputAgetWide",
        aput_aget_x_patterns(OPCODE_APUT_WIDE, OPCODE_AGET_WIDE,
                             move_result_pseudo_wide),
        aput_x_patterns(OPCODE_APUT_WIDE)},
       {"Replace_AputAgetObject",
        aput_aget_x_patterns(OPCODE_APUT_OBJECT, OPCODE_AGET_OBJECT,
                             move_result_pseudo_object),
        aput_x_patterns(OPCODE_APUT_OBJECT)},
       {"Replace_AputAgetShort",
        aput_aget_x_patterns(OPCODE_APUT_SHORT, OPCODE_AGET_SHORT,
                             move_result_pseudo),
        aput_x_patterns(OPCODE_APUT_SHORT)},
       {"Replace_AputAgetChar",
        aput_aget_x_patterns(OPCODE_APUT_CHAR, OPCODE_AGET_CHAR,
                             move_result_pseudo),
        aput_x_patterns(OPCODE_APUT_CHAR)},
       {"Replace_AputAgetByte",
        aput_aget_x_patterns(OPCODE_APUT_BYTE, OPCODE_AGET_BYTE,
                             move_result_pseudo),
        aput_x_patterns(OPCODE_APUT_BYTE)},
       {"Replace_AputAgetBoolean",
        aput_aget_x_patterns(OPCODE_APUT_BOOLEAN, OPCODE_AGET_BOOLEAN,
                             move_result_pseudo),
        aput_x_patterns(OPCODE_APUT_BOOLEAN)}});
  return *kAputAgetPatterns;
}

const std::vector<Pattern>& get_putget_patterns() {
  static const auto* kPutGetPatterns = new std::vector<Pattern>(
      {{"Replace_PutGet",
        put_get_x_patterns(OPCODE_IPUT, OPCODE_IGET, move_result_pseudo),
        put_x_patterns(OPCODE_IPUT), second_get_non_volatile},
       {"Replace_PutGetWide",
        put_get_x_patterns(OPCODE_IPUT_WIDE, OPCODE_IGET_WIDE,
                           move_result_pseudo_wide),
        put_x_patterns(OPCODE_IPUT_WIDE), second_get_non_volatile},
       {"Replace_PutGetObject",
        put_get_x_patterns(OPCODE_IPUT_OBJECT, OPCODE_IGET_OBJECT,
                           move_result_pseudo_object),
        put_x_patterns(OPCODE_IPUT_OBJECT), second_get_non_volatile},
       {"Replace_PutGetShort",
        put_get_x_patterns(OPCODE_IPUT_SHORT, OPCODE_IGET_SHORT,
                           move_result_pseudo),
        put_x_patterns(OPCODE_IPUT_SHORT), second_get_non_volatile},
       {"Replace_PutGetChar",
        put_get_x_patterns(OPCODE_IPUT_CHAR, OPCODE_IGET_CHAR,
                           move_result_pseudo),
        put_x_patterns(OPCODE_IPUT_CHAR), second_get_non_volatile},
       {"Replace_PutGetByte",
        put_get_x_patterns(OPCODE_IPUT_BYTE, OPCODE_IGET_BYTE,
                           move_result_pseudo),
        put_x_patterns(OPCODE_IPUT_BYTE), second_get_non_volatile},
       {"Replace_PutGetBoolean",
        put_get_x_patterns(OPCODE_IPUT_BOOLEAN, OPCODE_IGET_BOOLEAN,
                           move_result_pseudo),
        put_x_patterns(OPCODE_IPUT_BOOLEAN), second_get_non_volatile},

       {"Replace_StaticPutGet",
        put_get_x_patterns(OPCODE_SPUT, OPCODE_SGET, move_result_pseudo),
        put_x_patterns(OPCODE_SPUT), second_get_non_volatile},
       {"Replace_StaticPutGetWide",
        put_get_x_patterns(OPCODE_SPUT_WIDE, OPCODE_SGET_WIDE,
                           move_result_pseudo_wide),
        put_x_patterns(OPCODE_SPUT_WIDE), second_get_non_volatile},
       {"Replace_StaticPutGetObject",
        put_get_x_patterns(OPCODE_SPUT_OBJECT, OPCODE_SGET_OBJECT,
                           move_result_pseudo_object),
        put_x_patterns(OPCODE_SPUT_OBJECT), second_get_non_volatile},
       {"Replace_StaticPutGetShort",
        put_get_x_patterns(OPCODE_SPUT_SHORT, OPCODE_SGET_SHORT,
                           move_result_pseudo),
        put_x_patterns(OPCODE_SPUT_SHORT), second_get_non_volatile},
       {"Replace_StaticPutGetChar",
        put_get_x_patterns(OPCODE_SPUT_CHAR, OPCODE_SGET_CHAR,
                           move_result_pseudo),
        put_x_patterns(OPCODE_SPUT_CHAR), second_get_non_volatile},
       {"Replace_StaticPutGetByte",
        put_get_x_patterns(OPCODE_SPUT_BYTE, OPCODE_SGET_BYTE,
                           move_result_pseudo),
        put_x_patterns(OPCODE_SPUT_BYTE), second_get_non_volatile},
       {"Replace_StaticPutGetBoolean",
        put_get_x_patterns(OPCODE_SPUT_BOOLEAN, OPCODE_SGET_BOOLEAN,
                           move_result_pseudo),
        put_x_patterns(OPCODE_SPUT_BOOLEAN), second_get_non_volatile}});
  return *kPutGetPatterns;
}

template <int64_t VALUE>
static bool first_instruction_literal_is(const Matcher& m) {
  if (m.matched_instructions.empty()) {
    return false;
  }
  return m.matched_instructions.front()->get_literal() == VALUE;
}

static bool first_instruction_literal_is_power_of_two(const Matcher& m) {
  if (m.matched_instructions.empty()) {
    return false;
  }
  auto literal = m.matched_instructions.front()->get_literal();
  return literal > 0 && ((literal & (literal - 1)) == 0);
}

DexPattern mul_lit(Register src, Register dst) {
  return {{OPCODE_MUL_INT_LIT8, OPCODE_MUL_INT_LIT16}, {src}, {dst}};
}

DexPattern mul_literal_kind(Register src, Register dst, Literal lit) {
  return {{OPCODE_MUL_INT_LIT8, OPCODE_MUL_INT_LIT16}, {src}, {dst}, {lit}};
}

std::vector<DexPattern> div_lit(Register src, Register dst) {
  return {{{OPCODE_DIV_INT_LIT8, OPCODE_DIV_INT_LIT16}, {src}, {}},
          {{IOPCODE_MOVE_RESULT_PSEUDO}, {}, {dst}}};
}

std::vector<DexPattern> div_literal_kind(Register src,
                                         Register dst,
                                         Literal lit) {
  return {{{OPCODE_DIV_INT_LIT8, OPCODE_DIV_INT_LIT16}, {src}, {}, {lit}},
          {{IOPCODE_MOVE_RESULT_PSEUDO}, {}, {dst}}};
}

DexPattern add_lit(Register src, Register dst) {
  return {{OPCODE_ADD_INT_LIT8, OPCODE_ADD_INT_LIT16}, {src}, {dst}};
}

const std::vector<Pattern>& get_arith_patterns() {
  static const std::vector<Pattern> kArithPatterns = {
      // Replace *1 with move
      {"Arith_MulLit_Pos1",
       {mul_lit(Register::A, Register::B)},
       {// x = y * 1 -> x = y
        {{OPCODE_MOVE}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<1>},

      // Replace /1 with move
      {"Arith_DivLit_Pos1",
       {div_lit(Register::A, Register::B)},
       {// x = y * 1 -> x = y
        {{OPCODE_MOVE}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<1>},

      // Replace multiplies by -1 with negation
      {"Arith_MulLit_Neg1",
       {mul_lit(Register::A, Register::B)},
       {// Eliminates the literal-carrying halfword
        {{OPCODE_NEG_INT}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<-1>},

      // Replace divides by -1 with negation
      {"Arith_DivLit_Neg1",
       {div_lit(Register::A, Register::B)},
       {// Eliminates the literal-carrying halfword
        {{OPCODE_NEG_INT}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<-1>},

      // Replace +0 with moves
      {"Arith_AddLit_0",
       {add_lit(Register::A, Register::B)},
       {// Eliminates the literal-carrying halfword
        {{OPCODE_MOVE}, {Register::A}, {Register::B}}},
       first_instruction_literal_is<0>},

      // Replace mul 2^n with shl n
      {"Arith_MulLit_Power2",
       {mul_literal_kind(Register::A, Register::B,
                         Literal::Mul_Div_To_Shift_Log2)},
       {{{OPCODE_SHL_INT_LIT8},
         {Register::A},
         {Register::B},
         Literal::Mul_Div_To_Shift_Log2}},
       first_instruction_literal_is_power_of_two},

      // Replace div 2^n with shr n
      {"Arith_DivLit_Power2",
       {div_literal_kind(Register::A, Register::B,
                         Literal::Mul_Div_To_Shift_Log2)},
       {{{OPCODE_SHR_INT_LIT8},
         {Register::A},
         {Register::B},
         Literal::Mul_Div_To_Shift_Log2}},
       first_instruction_literal_is_power_of_two},
  };
  return kArithPatterns;
}

const DexPattern invoke_class_get_simple_name() {
  return {{OPCODE_INVOKE_VIRTUAL,
           OPCODE_INVOKE_SUPER,
           OPCODE_INVOKE_DIRECT,
           OPCODE_INVOKE_STATIC,
           OPCODE_INVOKE_INTERFACE},
          {Register::A},
          {},
          DexMethod::make_method(
              "Ljava/lang/Class;", "getSimpleName", "Ljava/lang/String;", {})};
}

DexPattern const_class(Type type) {
  return {{OPCODE_CONST_CLASS}, {}, {}, type};
};

const std::vector<Pattern>& get_func_patterns() {
  static const std::vector<Pattern> kFuncPatterns = {
      {"Remove_LangClass_GetSimpleName",
       {const_class(Type::A),
        move_result_pseudo_object(Register::A),
        invoke_class_get_simple_name(),
        move_result_object(Register::B)},
       {DexPattern::copy_matched_instruction(0), // const_class (maybe dead)
        move_result_pseudo_object(Register::A),
        const_string(String::Type_A_get_simple_name),
        move_result_pseudo_object(Register::B)}},
  };
  return kFuncPatterns;
}

const std::vector<std::vector<Pattern>>& get_all_patterns() {
  static const std::vector<std::vector<Pattern>>& kAllPatterns = {
      get_string_patterns(), get_arith_patterns(),  get_func_patterns(),
      get_nop_patterns(),    get_putget_patterns(), get_aputaget_patterns()};

  return kAllPatterns;
}

} // namespace patterns

template <typename T>
bool contains(const std::vector<T>& vec, const T& value) {
  return std::find(vec.begin(), vec.end(), value) != vec.end();
}

class PeepholeOptimizer {
 private:
  std::vector<Matcher> m_matchers;
  std::vector<size_t> m_stats;
  PassManager& m_mgr;
  int m_stats_removed = 0;
  int m_stats_inserted = 0;

 public:
  explicit PeepholeOptimizer(
      PassManager& mgr, const std::vector<std::string>& disabled_peepholes)
      : m_mgr(mgr) {
    for (const auto& pattern_list : patterns::get_all_patterns()) {
      for (const Pattern& pattern : pattern_list) {
        if (!contains(disabled_peepholes, pattern.name)) {
          m_matchers.emplace_back(pattern);
        } else {
          TRACE(PEEPHOLE,
                2,
                "not running disabled peephole opt %s\n",
                pattern.name.c_str());
        }
      }
    }
    m_stats.resize(m_matchers.size(), 0);
  }

  PeepholeOptimizer(const PeepholeOptimizer&) = delete;
  PeepholeOptimizer& operator=(const PeepholeOptimizer&) = delete;

  void peephole(DexMethod* method) {
    auto code = method->get_code();
    code->build_cfg(/* editable */ false);

    // do optimizations one at a time
    // so they can match on the same pattern without interfering
    for (size_t i = 0; i < m_matchers.size(); ++i) {
      auto& matcher = m_matchers[i];
      std::vector<IRInstruction*> deletes;
      std::vector<std::pair<IRInstruction*, std::vector<IRInstruction*>>>
          inserts;
      const auto& blocks = code->cfg().blocks();
      for (const auto& block : blocks) {
        // Currently, all patterns do not span over multiple basic blocks. So
        // reset all matching states on visiting every basic block.
        matcher.reset();

        for (auto& mei : InstructionIterable(block)) {
          if (!matcher.try_match(mei.insn)) {
            continue;
          }
          m_stats.at(i)++;
          TRACE(PEEPHOLE, 7, "PATTERN %s MATCHED!\n",
                matcher.pattern.name.c_str());
          for (auto insn : matcher.matched_instructions) {
            if (opcode::is_move_result_pseudo(insn->opcode())) {
              continue;
            }
            deletes.push_back(insn);
          }

          auto replace = matcher.get_replacements();
          for (const auto& r : replace) {
            TRACE(PEEPHOLE, 8, "-- %s\n", SHOW(r));
          }

          m_stats_inserted += replace.size();
          m_stats_removed += matcher.match_index;

          inserts.emplace_back(mei.insn, replace);
          matcher.reset();
        }
      }

      for (auto& pair : inserts) {
        std::vector<IRInstruction*> vec{begin(pair.second), end(pair.second)};
        code->insert_after(pair.first, vec);
      }
      for (auto& insn : deletes) {
        code->remove_opcode(insn);
      }
    }
  }

  void print_stats() {
    TRACE(PEEPHOLE, 1, "%d instructions removed\n", m_stats_removed);
    TRACE(PEEPHOLE, 1, "%d instructions inserted\n", m_stats_inserted);
    TRACE(PEEPHOLE,
          1,
          "%d net instruction change\n",
          m_stats_inserted - m_stats_removed);
    int num_patterns_matched = 0;
    for (size_t i = 0; i < m_matchers.size(); ++i) {
      num_patterns_matched +=
          m_mgr.get_metric(m_matchers[i].pattern.name.c_str());
    }
    TRACE(PEEPHOLE,
          1,
          "%lu patterns matched and replaced\n",
          num_patterns_matched);
    TRACE(PEEPHOLE, 5, "Detailed pattern match stats:\n");
    for (size_t i = 0; i < m_matchers.size(); ++i) {
      std::string current_pattern_name = m_matchers[i].pattern.name;
      TRACE(PEEPHOLE,
            5,
            "%s: %d\n",
            current_pattern_name.c_str(),
            m_mgr.get_metric(current_pattern_name.c_str()));
    }
  }

  void run_method(DexMethod* m) {
    if (m->get_code()) {
      peephole(m);
    }
  }

  void incr_all_metrics() {
    for (size_t i = 0; i < m_matchers.size(); i++) {
      m_mgr.incr_metric(m_matchers[i].pattern.name.c_str(), m_stats[i]);
    }
  }
};
}

void PeepholePass::run_pass(DexStoresVector& stores,
                            ConfigFiles& /*cfg*/,
                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  std::vector<std::unique_ptr<PeepholeOptimizer>> helpers;
  auto wq = WorkQueue<DexClass*, PeepholeOptimizer*, std::nullptr_t>(
      [&](WorkerState<DexClass*, PeepholeOptimizer*, std::nullptr_t>* state,
          DexClass* cls) {
        PeepholeOptimizer* ph = state->get_data();
        for (auto dmethod : cls->get_dmethods()) {
          TraceContext context(dmethod->get_deobfuscated_name());
          ph->run_method(dmethod);
        }
        for (auto vmethod : cls->get_vmethods()) {
          TraceContext context(vmethod->get_deobfuscated_name());
          ph->run_method(vmethod);
        }
        return nullptr;
      },
      [](std::nullptr_t, std::nullptr_t) { return nullptr; }, // reducer
      [&](unsigned int /*thread_index*/) { // data initializer
        helpers.emplace_back(std::make_unique<PeepholeOptimizer>(
            mgr, config.disabled_peepholes));
        return helpers.back().get();
      },
      std::thread::hardware_concurrency() / 2);
  for (auto* cls : scope) {
    wq.add_item(cls);
  }
  wq.run_all();

  for (const auto& helper : helpers) {
    helper->incr_all_metrics();
  }

  if (!contains<std::string>(config.disabled_peepholes,
                             RedundantCheckCastRemover::get_name())) {
    RedundantCheckCastRemover(mgr, scope).run();
  } else {
    TRACE(PEEPHOLE,
          2,
          "not running disabled peephole opt %s\n",
          RedundantCheckCastRemover::get_name().c_str());
  }
}

static PeepholePass s_pass;
