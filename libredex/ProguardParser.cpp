/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <iostream>
#include <vector>

#include "Debug.h"
#include "Macros.h"
#include "ProguardLexer.h"
#include "ProguardMap.h"
#include "ProguardParser.h"
#include "ProguardRegex.h"
#include "ReadMaybeMapped.h"

namespace keep_rules {
namespace proguard_parser {
namespace {

struct TokenIndex {
  const std::vector<Token>& vec;
  std::vector<Token>::const_iterator it;

  TokenIndex(const std::vector<Token>& vec,
             std::vector<Token>::const_iterator it)
      : vec(vec), it(it) {}

  void skip_comments() {
    while (it != vec.end() && it->type == TokenType::comment) {
      ++it;
    }
  }

  void next() {
    redex_assert(it != vec.end());
    ++it;
    skip_comments();
  }

  std::string_view data() const { return it->data; }
  std::string str() const { return std::string{it->data}; }
  std::string show() const { return it->show(); }

  size_t line() const { return it->line; }

  TokenType type() const { return it->type; }

  std::string show_context(size_t lines) const {
    redex_assert(it != vec.end());

    size_t this_line = line();
    auto start_it = it;
    while (start_it != vec.begin() && start_it->line >= this_line - lines) {
      --start_it;
    }
    if (start_it->line < this_line - lines) {
      ++start_it;
    }

    auto end_it = it;
    while (end_it != vec.end() && end_it->line <= this_line + lines) {
      ++end_it;
    }

    std::string ret;
    std::optional<size_t> last_line = std::nullopt;
    bool new_line = true;
    for (auto show_it = start_it; show_it != end_it; ++show_it) {
      if (!last_line || last_line != show_it->line) {
        if (last_line) {
          ret.append("\n");
        }
        ret.append(std::to_string(show_it->line));
        ret.append(": ");
        last_line = show_it->line;
        new_line = true;
      }
      if (!new_line) {
        ret.append(" ");
      }

      if (show_it == it) {
        ret.append("!>");
      }

      ret.append(show_it->show());

      if (show_it == it) {
        ret.append("<!");
      }

      new_line = false;
    }

    return ret;
  }
};

bool parse_boolean_command(TokenIndex& idx,
                           TokenType boolean_option,
                           bool* option,
                           bool value) {
  if (idx.type() != boolean_option) {
    return false;
  }
  idx.next();
  *option = value;
  return true;
}

void skip_to_next_command(TokenIndex& idx) {
  while ((idx.type() != TokenType::eof_token) && (!idx.it->is_command())) {
    idx.next();
  }
}

bool parse_single_filepath_command(TokenIndex& idx,
                                   TokenType filepath_command_token,
                                   std::string* filepath) {
  if (idx.type() == filepath_command_token) {
    unsigned int line_number = idx.line();
    idx.next(); // Consume the command token.
    // Fail without consumption if this is an end of file token.
    if (idx.type() == TokenType::eof_token) {
      std::cerr
          << "Expecting at least one file as an argument but found end of "
             "file at line "
          << line_number << std::endl
          << idx.show_context(2) << std::endl;
      return true;
    }
    // Fail without consumption if this is a command token.
    if (idx.it->is_command()) {
      std::cerr << "Expecting a file path argument but got command "
                << idx.show() << " at line  " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      return true;
    }
    // Parse the filename.
    if (idx.type() != TokenType::filepath) {
      std::cerr << "Expected a filepath but got " << idx.show() << " at line "
                << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      return true;
    }
    *filepath = idx.str();
    idx.next(); // Consume the filepath token
    return true;
  }
  return false;
}

template <bool kOptional = false>
void parse_filepaths(TokenIndex& idx, std::vector<std::string>* into) {
  std::vector<std::string> filepaths;
  if (idx.type() != TokenType::filepath) {
    if (!kOptional) {
      std::cerr << "Expected filepath but got " << idx.show() << " at line "
                << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
    }
    return;
  }
  while (idx.type() == TokenType::filepath) {
    into->push_back(idx.str());
    idx.next();
  }
}

bool parse_filepath_command(TokenIndex& idx,
                            TokenType filepath_command_token,
                            const std::string& basedir,
                            std::vector<std::string>* filepaths) {
  if (idx.type() == filepath_command_token) {
    unsigned int line_number = idx.line();
    idx.next(); // Consume the command token.
    // Fail without consumption if this is an end of file token.
    if (idx.type() == TokenType::eof_token) {
      std::cerr
          << "Expecting at least one file as an argument but found end of "
             "file at line "
          << line_number << std::endl;
      return true;
    }
    // Fail without consumption if this is a command token.
    if (idx.it->is_command()) {
      std::cerr << "Expecting a file path argument but got command "
                << idx.show() << " at line  " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      return true;
    }
    // Parse the filename.
    if (idx.type() != TokenType::filepath) {
      std::cerr << "Expected a filepath but got " << idx.show() << " at line "
                << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      return true;
    }
    parse_filepaths(idx, filepaths);
    return true;
  }
  return false;
}

bool parse_optional_filepath_command(TokenIndex& idx,
                                     TokenType filepath_command_token,
                                     std::vector<std::string>* filepaths) {
  if (idx.type() != filepath_command_token) {
    return false;
  }
  idx.next(); // Consume the command token.
  // Parse an optional filepath argument.
  parse_filepaths</*kOptional=*/true>(idx, filepaths);
  return true;
}

bool parse_jars(TokenIndex& idx,
                TokenType jar_token,
                const std::string& basedir,
                std::vector<std::string>* jars) {
  if (idx.type() == jar_token) {
    unsigned int line_number = idx.line();
    idx.next(); // Consume the jar token.
    // Fail without consumption if this is an end of file token.
    if (idx.type() == TokenType::eof_token) {
      std::cerr
          << "Expecting at least one file as an argument but found end of "
             "file at line "
          << line_number << std::endl
          << idx.show_context(2) << std::endl;
      return true;
    }
    // Parse the list of filenames.
    parse_filepaths(idx, jars);
    return true;
  }
  return false;
}

bool parse_dontusemixedcaseclassnames(TokenIndex& idx,
                                      bool* dontusemixedcaseclassnames) {
  if (idx.type() != TokenType::dontusemixedcaseclassnames_token) {
    return false;
  }
  *dontusemixedcaseclassnames = true;
  idx.next();
  return true;
}

bool parse_dontpreverify(TokenIndex& idx, bool* dontpreverify) {
  if (idx.type() != TokenType::dontpreverify_token) {
    return false;
  }
  *dontpreverify = true;
  idx.next();
  return true;
}

bool parse_verbose(TokenIndex& idx, bool* verbose) {
  if (idx.type() != TokenType::verbose_token) {
    return false;
  }
  *verbose = true;
  idx.next();
  return true;
}

bool parse_bool_command(TokenIndex& idx,
                        TokenType bool_command_token,
                        bool new_value,
                        bool* bool_value) {
  if (idx.type() == bool_command_token) {
    idx.next(); // Consume the boolean command token.
    *bool_value = new_value;
    return true;
  }
  return false;
}

bool parse_repackageclasses(TokenIndex& idx) {
  if (idx.type() != TokenType::repackageclasses) {
    return false;
  }
  // Ignore repackageclasses.
  idx.next();
  if (idx.type() == TokenType::identifier) {
    std::cerr << "Ignoring -repackageclasses " << idx.data() << std::endl
              << idx.show_context(2) << std::endl;
    idx.next();
  }
  return true;
}

bool parse_target(TokenIndex& idx, std::string* target_version) {
  if (idx.type() == TokenType::target) {
    idx.next(); // Consume the target command token.
    // Check to make sure the next TokenType is a version token.
    if (idx.type() != TokenType::target_version_token) {
      std::cerr << "Expected a target version but got " << idx.show()
                << " at line " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      return true;
    }
    *target_version = idx.str();
    // Consume the target version token.
    idx.next();
    return true;
  }
  return false;
}

bool parse_allowaccessmodification(TokenIndex& idx,
                                   bool* allowaccessmodification) {
  if (idx.type() != TokenType::allowaccessmodification_token) {
    return false;
  }
  idx.next();
  *allowaccessmodification = true;
  return true;
}

bool parse_filter_list_command(TokenIndex& idx,
                               TokenType filter_command_token,
                               std::vector<std::string>* filters) {
  if (idx.type() != filter_command_token) {
    return false;
  }
  idx.next();
  while (idx.type() == TokenType::filter_pattern) {
    filters->push_back(idx.str());
    idx.next();
  }
  return true;
}

bool parse_optimizationpasses_command(TokenIndex& idx) {
  if (idx.type() != TokenType::optimizationpasses) {
    return false;
  }
  idx.next();
  // Comsume the next token.
  idx.next();
  return true;
}

bool is_modifier(TokenType tok) {
  return tok == TokenType::includedescriptorclasses_token ||
         tok == TokenType::allowshrinking_token ||
         tok == TokenType::allowoptimization_token ||
         tok == TokenType::allowobfuscation_token;
}

bool parse_modifiers(TokenIndex& idx, KeepSpec* keep) {
  while (idx.type() == TokenType::comma) {
    idx.next();
    if (!is_modifier(idx.type())) {
      std::cerr << "Expected keep option modifier but found : " << idx.show()
                << " at line number " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      return false;
    }
    switch (idx.type()) {
    case TokenType::includedescriptorclasses_token:
      keep->includedescriptorclasses = true;
      break;
    case TokenType::allowshrinking_token:
      keep->allowshrinking = true;
      break;
    case TokenType::allowoptimization_token:
      keep->allowoptimization = true;
      break;
    case TokenType::allowobfuscation_token:
      keep->allowobfuscation = true;
      break;
    default:
      break;
    }
    idx.next();
  }
  return true;
}

DexAccessFlags process_access_modifier(TokenType type, bool* is_access_flag) {
  *is_access_flag = true;
  switch (type) {
  case TokenType::publicToken:
    return ACC_PUBLIC;
  case TokenType::privateToken:
    return ACC_PRIVATE;
  case TokenType::final:
    return ACC_FINAL;
  case TokenType::abstract:
    return ACC_ABSTRACT;
  case TokenType::synthetic:
    return ACC_SYNTHETIC;
  case TokenType::staticToken:
    return ACC_STATIC;
  case TokenType::volatileToken:
    return ACC_VOLATILE;
  case TokenType::native:
    return ACC_NATIVE;
  case TokenType::protectedToken:
    return ACC_PROTECTED;
  case TokenType::transient:
    return ACC_TRANSIENT;
  default:
    *is_access_flag = false;
    return ACC_PUBLIC;
  }
}

bool is_negation_or_class_access_modifier(TokenType type) {
  switch (type) {
  case TokenType::notToken:
  case TokenType::publicToken:
  case TokenType::privateToken:
  case TokenType::protectedToken:
  case TokenType::final:
  case TokenType::abstract:
  case TokenType::synthetic:
  case TokenType::native:
  case TokenType::staticToken:
  case TokenType::volatileToken:
  case TokenType::transient:
    return true;
  default:
    return false;
  }
}

std::string parse_annotation_type(TokenIndex& idx) {
  if (idx.type() != TokenType::annotation_application) {
    return "";
  }
  idx.next();
  if (idx.type() != TokenType::identifier) {
    std::cerr << "Expecting a class identifier after @ but got " << idx.show()
              << " at line " << idx.line() << std::endl
              << idx.show_context(2) << std::endl;
    return "";
  }
  const auto& typ = idx.data();
  idx.next();
  return convert_wildcard_type(typ);
}

bool is_access_flag_set(const DexAccessFlags accessFlags,
                        const DexAccessFlags checkingFlag) {
  return accessFlags & checkingFlag;
}

void set_access_flag(DexAccessFlags& accessFlags,
                     const DexAccessFlags settingFlag) {
  accessFlags = accessFlags | settingFlag;
}

bool parse_access_flags(TokenIndex& idx,
                        DexAccessFlags& setFlags_,
                        DexAccessFlags& unsetFlags_) {
  bool negated = false;
  while (is_negation_or_class_access_modifier(idx.type())) {
    // Copy the iterator so we can peek and see if the next TokenType is an
    // access token; we don't want to modify the main iterator otherwise
    auto access_it = idx.it;
    if (idx.type() == TokenType::notToken) {
      negated = true;
      ++access_it;
    }
    bool ok;
    DexAccessFlags access_flag = process_access_modifier(access_it->type, &ok);
    if (ok) {
      idx.it = ++access_it;
      if (negated) {
        if (is_access_flag_set(setFlags_, access_flag)) {
          std::cerr << "Access flag " << idx.show()
                    << " occurs with conflicting settings at line "
                    << idx.line() << std::endl
                    << idx.show_context(2) << std::endl;
          return false;
        }
        set_access_flag(unsetFlags_, access_flag);
        negated = false;
      } else {
        if (is_access_flag_set(unsetFlags_, access_flag)) {
          std::cerr << "Access flag " << idx.show()
                    << " occurs with conflicting settings at line "
                    << idx.line() << std::endl
                    << idx.show_context(2) << std::endl;
          return false;
        }
        set_access_flag(setFlags_, access_flag);
        negated = false;
      }
    } else {
      break;
    }
  }
  return true;
}

/*
 * Parse [!](class|interface|enum|@interface).
 */
bool parse_class_token(TokenIndex& idx,
                       DexAccessFlags& setFlags_,
                       DexAccessFlags& unsetFlags_) {
  bool negated = false;
  if (idx.type() == TokenType::notToken) {
    negated = true;
    idx.next();
  }
  // Make sure the next keyword is interface, class, enum.
  switch (idx.type()) {
  case TokenType::interface:
    set_access_flag(negated ? unsetFlags_ : setFlags_, ACC_INTERFACE);
    break;
  case TokenType::enumToken:
    set_access_flag(negated ? unsetFlags_ : setFlags_, ACC_ENUM);
    break;
  case TokenType::annotation:
    set_access_flag(negated ? unsetFlags_ : setFlags_, ACC_ANNOTATION);
    break;
  case TokenType::classToken:
    break;
  default:
    std::cerr << "Expected interface, class or enum but got " << idx.show()
              << " at line number " << idx.line() << std::endl
              << idx.show_context(2) << std::endl;
    return false;
  }
  idx.next();
  return true;
}

// Consume an expected token, indicating if that TokenType was found.
// If some other TokenType is found, then it is not consumed and false
// is returned.
bool consume_token(TokenIndex& idx, const TokenType& tok) {
  if (idx.type() != tok) {
    std::cerr << "Unexpected TokenType " << idx.show() << std::endl
              << idx.show_context(2) << std::endl;
    return false;
  }
  idx.next();
  return true;
}

// Consume an expected semicolon, complaining if one was not found.
void gobble_semicolon(TokenIndex& idx, bool* ok) {
  *ok = consume_token(idx, TokenType::semiColon);
  if (!*ok) {
    std::cerr << "Expecting a semicolon but found " << idx.show() << " at line "
              << idx.line() << std::endl
              << idx.show_context(2) << std::endl;
    return;
  }
}

void skip_to_semicolon(TokenIndex& idx) {
  while ((idx.type() != TokenType::semiColon) &&
         (idx.type() != TokenType::eof_token)) {
    idx.next();
  }
  if (idx.type() == TokenType::semiColon) {
    idx.next();
  }
}

void parse_member_specification(TokenIndex& idx,
                                ClassSpecification* class_spec,
                                bool* ok) {
  MemberSpecification member_specification;
  *ok = true;
  member_specification.annotationType = parse_annotation_type(idx);
  if (!parse_access_flags(idx,
                          member_specification.requiredSetAccessFlags,
                          member_specification.requiredUnsetAccessFlags)) {
    // There was a problem parsing the access flags. Return an empty class spec
    // for now.
    std::cerr << "Problem parsing access flags for member specification.\n";
    *ok = false;
    skip_to_semicolon(idx);
    return;
  }
  // The next TokenType better be an identifier.
  if (idx.type() != TokenType::identifier) {
    std::cerr << "Expecting field or member specification but got "
              << idx.show() << " at line " << idx.line() << std::endl
              << idx.show_context(2) << std::endl;
    *ok = false;
    skip_to_semicolon(idx);
    return;
  }
  const auto& ident = idx.data();
  // Check for "*".
  if (ident == "*") {
    member_specification.name = "";
    member_specification.descriptor = "";
    idx.next();
    gobble_semicolon(idx, ok);
    class_spec->methodSpecifications.push_back(member_specification);
    class_spec->fieldSpecifications.push_back(member_specification);
    return;
  }
  // Check for <methods>
  if (ident == "<methods>") {
    member_specification.name = "";
    member_specification.descriptor = "";
    idx.next();
    gobble_semicolon(idx, ok);
    class_spec->methodSpecifications.push_back(member_specification);
    return;
  }
  // Check for <fields>
  if (ident == "<fields>") {
    member_specification.name = "";
    member_specification.descriptor = "";
    idx.next();
    gobble_semicolon(idx, ok);
    class_spec->fieldSpecifications.push_back(member_specification);
    return;
  }
  // Check for <init>
  if (ident == "<init>") {
    member_specification.name = "<init>";
    member_specification.descriptor = "V";
    set_access_flag(member_specification.requiredSetAccessFlags,
                    ACC_CONSTRUCTOR);
    idx.next();
  } else {
    // This TokenType is the type for the member specification.
    if (idx.type() != TokenType::identifier) {
      std::cerr << "Expecting type identifier but got " << idx.show()
                << " at line " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      *ok = false;
      skip_to_semicolon(idx);
      return;
    }
    const auto& typ = idx.data();
    idx.next();
    member_specification.descriptor = convert_wildcard_type(typ);
    if (idx.type() != TokenType::identifier) {
      std::cerr << "Expecting identifier name for class member but got "
                << idx.show() << " at line " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      *ok = false;
      skip_to_semicolon(idx);
      return;
    }
    member_specification.name = idx.str();
    idx.next();
  }
  // Check to see if this is a method specification.
  if (idx.type() == TokenType::openBracket) {
    consume_token(idx, TokenType::openBracket);
    std::string arg = "(";
    while (true) {
      // If there is a ")" next we are done.
      if (idx.type() == TokenType::closeBracket) {
        consume_token(idx, TokenType::closeBracket);
        break;
      }
      if (idx.type() != TokenType::identifier) {
        std::cerr << "Expecting type identifier but got " << idx.show()
                  << " at line " << idx.line() << std::endl
                  << idx.show_context(2) << std::endl;
        *ok = false;
        return;
      }
      const auto& typ = idx.data();
      consume_token(idx, TokenType::identifier);
      arg += convert_wildcard_type(typ);
      // The next TokenType better be a comma or a closing bracket.
      if (idx.type() != TokenType::comma &&
          idx.type() != TokenType::closeBracket) {
        std::cerr << "Expecting comma or ) but got " << idx.show()
                  << " at line " << idx.line() << std::endl
                  << idx.show_context(2) << std::endl;
        *ok = false;
        return;
      }
      // If the next TokenType is a comma (rather than closing bracket) consume
      // it and check that it is followed by an identifier.
      if (idx.type() == TokenType::comma) {
        consume_token(idx, TokenType::comma);
        if (idx.type() != TokenType::identifier) {
          std::cerr << "Expecting type identifier after comma but got "
                    << idx.show() << " at line " << idx.line() << std::endl
                    << idx.show_context(2) << std::endl;
          *ok = false;
          return;
        }
      }
    }
    arg += ")";
    arg += member_specification.descriptor;
    member_specification.descriptor = arg;
  }
  // if with value, look for return
  if (idx.type() == TokenType::returns) {
    idx.next();
    const auto& rident = idx.data();
    if (rident == "true") {
      member_specification.return_value.value_type =
          AssumeReturnValue::ValueType::ValueBool;
      member_specification.return_value.value.v = 1;
      idx.next();
    }
    if (rident == "false") {
      member_specification.return_value.value_type =
          AssumeReturnValue::ValueType::ValueBool;
      member_specification.return_value.value.v = 0;
      idx.next();
    }
  }
  // Make sure member specification ends with a semicolon.
  gobble_semicolon(idx, ok);
  if (!ok) {
    return;
  }
  if (member_specification.descriptor[0] == '(') {
    class_spec->methodSpecifications.push_back(member_specification);
  } else {
    class_spec->fieldSpecifications.push_back(member_specification);
  }
}

void parse_member_specifications(TokenIndex& idx,
                                 ClassSpecification* class_spec,
                                 bool* ok) {
  if (idx.type() == TokenType::openCurlyBracket) {
    idx.next();
    while ((idx.type() != TokenType::closeCurlyBracket) &&
           (idx.type() != TokenType::eof_token)) {
      parse_member_specification(idx, class_spec, ok);
      if (!*ok) {
        // We failed to parse a member specification so skip to the next
        // semicolon.
        skip_to_semicolon(idx);
      }
    }
    if (idx.type() == TokenType::closeCurlyBracket) {
      idx.next();
    }
  }
}

bool member_comparison(const MemberSpecification& m1,
                       const MemberSpecification& m2) {
  return m1.name < m2.name;
}

std::string parse_class_name(TokenIndex& idx, bool* ok) {
  if (idx.type() != TokenType::identifier) {
    std::cerr << "Expected class name but got " << idx.show() << " at line "
              << idx.line() << std::endl
              << idx.show_context(2) << std::endl;
    *ok = false;
    return "";
  }
  auto name = idx.str();
  idx.next();
  return name;
}

void parse_class_names(
    TokenIndex& idx,
    bool* ok,
    std::vector<ClassSpecification::ClassNameSpec>& class_names) {
  bool negated{false};
  auto maybe_negated = [&]() {
    if (idx.type() == TokenType::notToken) {
      negated = true;
      idx.next();
    }
  };
  auto push_to = [&](std::string&& s) {
    class_names.emplace_back(s, negated);
    negated = false;
  };

  maybe_negated();
  push_to(parse_class_name(idx, ok));
  if (!*ok) {
    return;
  }

  // Maybe consume comma delimited list
  while (idx.type() == TokenType::comma) {
    // Consume comma
    idx.next();

    maybe_negated();
    push_to(parse_class_name(idx, ok));
    if (!*ok) {
      return;
    }
  }
}

ClassSpecification parse_class_specification(TokenIndex& idx, bool* ok) {
  ClassSpecification class_spec;
  *ok = true;
  class_spec.annotationType = parse_annotation_type(idx);
  if (!parse_access_flags(
          idx, class_spec.setAccessFlags, class_spec.unsetAccessFlags)) {
    // There was a problem parsing the access flags. Return an empty class spec
    // for now.
    std::cerr << "Problem parsing access flags for class specification.\n";
    *ok = false;
    return class_spec;
  }
  if (!parse_class_token(
          idx, class_spec.setAccessFlags, class_spec.unsetAccessFlags)) {
    *ok = false;
    return class_spec;
  }
  // Parse the class name(s).
  parse_class_names(idx, ok, class_spec.classNames);
  if (!*ok) {
    return class_spec;
  }
  // Parse extends/implements if present, treating implements like extends.
  if ((idx.type() == TokenType::extends) ||
      (idx.type() == TokenType::implements)) {
    idx.next();
    class_spec.extendsAnnotationType = parse_annotation_type(idx);
    if (idx.type() != TokenType::identifier) {
      std::cerr << "Expecting a class name after extends/implements but got "
                << idx.show() << " at line " << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      *ok = false;
      class_spec.extendsClassName = "";
    } else {
      class_spec.extendsClassName = idx.str();
    }
    idx.next();
  }
  // Parse the member specifications, if there are any
  parse_member_specifications(idx, &class_spec, ok);
  std::sort(class_spec.fieldSpecifications.begin(),
            class_spec.fieldSpecifications.end(),
            member_comparison);
  std::sort(class_spec.methodSpecifications.begin(),
            class_spec.methodSpecifications.end(),
            member_comparison);
  return class_spec;
}

bool parse_keep(TokenIndex& idx,
                TokenType keep_kind,
                KeepSpecSet* spec,
                bool mark_classes,
                bool mark_conditionally,
                bool allowshrinking,
                const std::string& filename,
                uint32_t line,
                bool* ok) {
  if (idx.type() == keep_kind) {
    idx.next(); // Consume the keep token
    auto keep = std::make_unique<KeepSpec>();
    keep->mark_classes = mark_classes;
    keep->mark_conditionally = mark_conditionally;
    keep->allowshrinking = allowshrinking;
    keep->source_filename = filename;
    keep->source_line = line;
    if (!parse_modifiers(idx, &*keep)) {
      skip_to_next_command(idx);
      return true;
    }
    keep->class_spec = parse_class_specification(idx, ok);
    spec->emplace(std::move(keep));
    return true;
  }
  return false;
}

void parse(const std::vector<Token>& vec,
           ProguardConfiguration* pg_config,
           Stats& stats,
           const std::string& filename) {
  bool ok;
  TokenIndex idx{vec, vec.begin()};
  while (idx.it != idx.vec.end()) {
    // Break out if we are at the end of the TokenType stream.
    if (idx.type() == TokenType::eof_token) {
      break;
    }
    if (idx.type() == TokenType::comment) {
      idx.next();
      continue;
    }
    uint32_t line = idx.line();
    if (!idx.it->is_command()) {
      std::cerr << "Expecting command but found " << idx.show() << " at line "
                << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      idx.next();
      skip_to_next_command(idx);
      ++stats.unknown_commands;
      continue;
    }

    // Input/Output Options
    if (parse_filepath_command(idx,
                               TokenType::include,
                               pg_config->basedirectory,
                               &pg_config->includes)) {
      continue;
    }
    if (parse_single_filepath_command(
            idx, TokenType::basedirectory, &pg_config->basedirectory)) {
      continue;
    }
    if (parse_jars(idx,
                   TokenType::injars,
                   pg_config->basedirectory,
                   &pg_config->injars)) {
      continue;
    }
    if (parse_jars(idx,
                   TokenType::outjars,
                   pg_config->basedirectory,
                   &pg_config->outjars)) {
      continue;
    }
    if (parse_jars(idx,
                   TokenType::libraryjars,
                   pg_config->basedirectory,
                   &pg_config->libraryjars)) {
      continue;
    }
    // -skipnonpubliclibraryclasses not supported
    if (idx.type() == TokenType::dontskipnonpubliclibraryclasses) {
      // Silenty ignore the dontskipnonpubliclibraryclasses option.
      idx.next();
      continue;
    }
    // -dontskipnonpubliclibraryclassmembers not supported
    if (parse_filepath_command(idx,
                               TokenType::keepdirectories,
                               pg_config->basedirectory,
                               &pg_config->keepdirectories)) {
      continue;
    }
    if (parse_target(idx, &pg_config->target_version)) {
      continue;
    }
    // -forceprocessing not supported

    // Keep Options
    if (parse_keep(idx,
                   TokenType::keep,
                   &pg_config->keep_rules,
                   true, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        ++stats.parse_errors;
      }
      continue;
    }
    if (parse_keep(idx,
                   TokenType::keepclassmembers,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        ++stats.parse_errors;
      }
      continue;
    }
    if (parse_keep(idx,
                   TokenType::keepclasseswithmembers,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   true, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        ++stats.parse_errors;
      }
      continue;
    }
    if (parse_keep(idx,
                   TokenType::keepnames,
                   &pg_config->keep_rules,
                   true, // mark_classes
                   false, // mark_conditionally
                   true, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        ++stats.parse_errors;
      }
      continue;
    }
    if (parse_keep(idx,
                   TokenType::keepclassmembernames,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   true, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        ++stats.parse_errors;
      }
      continue;
    }
    if (parse_keep(idx,
                   TokenType::keepclasseswithmembernames,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   true, // mark_conditionally
                   true, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        ++stats.parse_errors;
      }
      continue;
    }
    if (parse_optional_filepath_command(
            idx, TokenType::printseeds, &pg_config->printseeds)) {
      continue;
    }

    // Shrinking Options
    if (parse_bool_command(
            idx, TokenType::dontshrink, false, &pg_config->shrink)) {
      continue;
    }
    if (parse_optional_filepath_command(
            idx, TokenType::printusage, &pg_config->printusage)) {
      continue;
    }

    // Optimization Options
    if (parse_boolean_command(
            idx, TokenType::dontoptimize, &pg_config->optimize, false)) {
      continue;
    }
    if (parse_filter_list_command(
            idx, TokenType::optimizations, &pg_config->optimization_filters)) {
      continue;
    }
    if (parse_optimizationpasses_command(idx)) {
      continue;
    }
    if (parse_keep(idx,
                   TokenType::assumenosideeffects,
                   &pg_config->assumenosideeffects_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      continue;
    }
    if (parse_keep(idx,
                   TokenType::whyareyoukeeping,
                   &pg_config->whyareyoukeeping_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      continue;
    }

    // Obfuscation Options
    if (idx.type() == TokenType::dontobfuscate) {
      pg_config->dontobfuscate = true;
      idx.next();
      continue;
    }
    // Redex ignores -dontskipnonpubliclibraryclasses
    if (idx.type() == TokenType::dontskipnonpubliclibraryclasses) {
      idx.next();
      continue;
    }
    if (parse_optional_filepath_command(
            idx, TokenType::printmapping, &pg_config->printmapping)) {
      continue;
    }
    if (parse_optional_filepath_command(idx,
                                        TokenType::printconfiguration,
                                        &pg_config->printconfiguration)) {
      continue;
    }

    if (parse_allowaccessmodification(idx,
                                      &pg_config->allowaccessmodification)) {
      continue;
    }
    if (parse_dontusemixedcaseclassnames(
            idx, &pg_config->dontusemixedcaseclassnames)) {
      continue;
    }
    if (parse_filter_list_command(
            idx, TokenType::keeppackagenames, &pg_config->keeppackagenames)) {
      continue;
    }
    if (parse_dontpreverify(idx, &pg_config->dontpreverify)) {
      continue;
    }
    if (parse_verbose(idx, &pg_config->verbose)) {
      continue;
    }
    if (parse_repackageclasses(idx)) {
      continue;
    }

    if (parse_filter_list_command(
            idx, TokenType::dontwarn, &pg_config->dontwarn)) {
      continue;
    }
    if (parse_filter_list_command(
            idx, TokenType::keepattributes, &pg_config->keepattributes)) {
      continue;
    }

    // Skip unknown token.
    if (idx.it->is_command()) {
      const auto& name = idx.data();
      // It is benign to drop -dontnote
      if (name != "dontnote") {
        std::cerr << "Unimplemented command (skipping): " << idx.show()
                  << " at line " << idx.line() << std::endl
                  << idx.show_context(2) << std::endl;
        ++stats.unimplemented;
      }
    } else {
      std::cerr << "Unexpected TokenType " << idx.show() << " at line "
                << idx.line() << std::endl
                << idx.show_context(2) << std::endl;
      ++stats.parse_errors;
    }
    idx.next();
    skip_to_next_command(idx);
  }
}

Stats parse(const std::string_view& config,
            ProguardConfiguration* pg_config,
            const std::string& filename) {
  Stats ret{};

  std::vector<Token> tokens = lex(config);
  bool ok = true;
  // Check for bad tokens.
  for (auto& tok : tokens) {
    if (tok.type == TokenType::unknownToken) {
      std::string spelling ATTRIBUTE_UNUSED = std::string(tok.data);
      ++ret.unknown_tokens;
      ok = false;
    }
    // std::cout << tok.show() << " at line " << tok.line << std::endl;
  }

  if (!ok) {
    std::cerr << "Found " << ret.unknown_tokens << " unkown tokens in "
              << filename << "\n";
    pg_config->ok = false;
    return ret;
  }

  parse(tokens, pg_config, ret, filename);
  if (ret.parse_errors == 0) {
    pg_config->ok = ok;
  } else {
    pg_config->ok = false;
    std::cerr << "Found " << ret.parse_errors << " parse errors in " << filename
              << "\n";
  }

  return ret;
}

} // namespace

Stats parse(std::istream& config,
            ProguardConfiguration* pg_config,
            const std::string& filename) {
  std::stringstream buffer;
  buffer << config.rdbuf();
  return parse(buffer.str(), pg_config, filename);
}

Stats parse_file(const std::string& filename,
                 ProguardConfiguration* pg_config) {
  Stats ret{};
  redex::read_file_with_contents(filename, [&](const char* data, size_t s) {
    std::string_view view(data, s);
    ret += parse(view, pg_config, filename);
    // Parse the included files.
    for (const auto& included_filename : pg_config->includes) {
      if (pg_config->already_included.find(included_filename) !=
          pg_config->already_included.end()) {
        continue;
      }
      pg_config->already_included.emplace(included_filename);
      ret += parse_file(included_filename, pg_config);
    }
  });
  return ret;
}

size_t remove_default_blocklisted_rules(ProguardConfiguration* pg_config) {
  // TODO: Make the set of excluded rules configurable.
  auto blocklisted_rules = R"(
  # The proguard-android-optimize.txt file that is bundled with the Android SDK
  # has a keep rule to prevent removal of all resource ID fields. This is likely
  # because ProGuard runs before aapt which can change the values of those
  # fields. Since this is no longer true in our case, this rule is redundant and
  # hampers our optimizations.
  #
  # I chose to exclude this rule instead of unmarking all resource IDs so that
  # if a resource ID really needs to be kept, the user can still keep it by
  # writing a keep rule that does a non-wildcard match.
  -keepclassmembers class **.R$* {
    public static <fields>;
  }

  # See keepclassnames.pro, or T1890454.
  -keepnames class *
)";
  return remove_blocklisted_rules(blocklisted_rules, pg_config);
}

size_t remove_blocklisted_rules(const std::string& rules,
                                ProguardConfiguration* pg_config) {
  ProguardConfiguration pg_config_blocklist;
  parse(rules, &pg_config_blocklist, "<internal blocklist>");
  size_t removed{0};
  pg_config->keep_rules.erase_if([&](const KeepSpec& ks) {
    for (const auto& blocklisted_ks : pg_config_blocklist.keep_rules) {
      if (ks == *blocklisted_ks) {
        removed++;
        return true;
      }
    }
    return false;
  });
  return removed;
}

} // namespace proguard_parser
} // namespace keep_rules
