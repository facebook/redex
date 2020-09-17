/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <iostream>
#include <vector>

#include "Macros.h"
#include "ProguardLexer.h"
#include "ProguardMap.h"
#include "ProguardParser.h"
#include "ProguardRegex.h"
#include "ReadMaybeMapped.h"

namespace keep_rules {
namespace proguard_parser {
namespace {

bool parse_boolean_command(std::vector<Token>::iterator* it,
                           TokenType boolean_option,
                           bool* option,
                           bool value) {
  if ((*it)->type != boolean_option) {
    return false;
  }
  ++(*it);
  *option = value;
  return true;
}

void skip_to_next_command(std::vector<Token>::iterator* it) {
  while (((*it)->type != TokenType::eof_token) && (!(*it)->is_command())) {
    ++(*it);
  }
}

bool parse_single_filepath_command(std::vector<Token>::iterator* it,
                                   TokenType filepath_command_token,
                                   std::string* filepath) {
  if ((*it)->type == filepath_command_token) {
    unsigned int line_number = (*it)->line;
    ++(*it); // Consume the command token.
    // Fail without consumption if this is an end of file token.
    if ((*it)->type == TokenType::eof_token) {
      std::cerr
          << "Expecting at least one file as an argument but found end of "
             "file at line "
          << line_number << std::endl;
      return true;
    }
    // Fail without consumption if this is a command token.
    if ((*it)->is_command()) {
      std::cerr << "Expecting a file path argument but got command "
                << (*it)->show() << " at line  " << (*it)->line << std::endl;
      return true;
    }
    // Parse the filename.
    if ((*it)->type != TokenType::filepath) {
      std::cerr << "Expected a filepath but got " << (*it)->show()
                << " at line " << (*it)->line << std::endl;
      return true;
    }
    *filepath = (*it)->data.to_string();
    ++(*it); // Consume the filepath token
    return true;
  }
  return false;
}

template <bool kOptional = false>
void parse_filepaths(std::vector<Token>::iterator* it,
                     std::vector<std::string>* into) {
  std::vector<std::string> filepaths;
  if ((*it)->type != TokenType::filepath) {
    if (!kOptional) {
      std::cerr << "Expected filepath but got " << (*it)->show() << " at line "
                << (*it)->line << std::endl;
    }
    return;
  }
  while ((*it)->type == TokenType::filepath) {
    into->push_back((*it)->data.to_string());
    ++(*it);
  }
}

bool parse_filepath_command(std::vector<Token>::iterator* it,
                            TokenType filepath_command_token,
                            const std::string& basedir,
                            std::vector<std::string>* filepaths) {
  if ((*it)->type == filepath_command_token) {
    unsigned int line_number = (*it)->line;
    ++(*it); // Consume the command token.
    // Fail without consumption if this is an end of file token.
    if ((*it)->type == TokenType::eof_token) {
      std::cerr
          << "Expecting at least one file as an argument but found end of "
             "file at line "
          << line_number << std::endl;
      return true;
    }
    // Fail without consumption if this is a command token.
    if ((*it)->is_command()) {
      std::cerr << "Expecting a file path argument but got command "
                << (*it)->show() << " at line  " << (*it)->line << std::endl;
      return true;
    }
    // Parse the filename.
    if ((*it)->type != TokenType::filepath) {
      std::cerr << "Expected a filepath but got " << (*it)->show()
                << " at line " << (*it)->line << std::endl;
      return true;
    }
    parse_filepaths(it, filepaths);
    return true;
  }
  return false;
}

bool parse_optional_filepath_command(std::vector<Token>::iterator* it,
                                     TokenType filepath_command_token,
                                     std::vector<std::string>* filepaths) {
  if ((*it)->type != filepath_command_token) {
    return false;
  }
  ++(*it); // Consume the command token.
  // Parse an optional filepath argument.
  parse_filepaths</*kOptional=*/true>(it, filepaths);
  return true;
}

bool parse_jars(std::vector<Token>::iterator* it,
                TokenType jar_token,
                const std::string& basedir,
                std::vector<std::string>* jars) {
  if ((*it)->type == jar_token) {
    unsigned int line_number = (*it)->line;
    ++(*it); // Consume the jar token.
    // Fail without consumption if this is an end of file token.
    if ((*it)->type == TokenType::eof_token) {
      std::cerr
          << "Expecting at least one file as an argument but found end of "
             "file at line "
          << line_number << std::endl;
      return true;
    }
    // Parse the list of filenames.
    parse_filepaths(it, jars);
    return true;
  }
  return false;
}

bool parse_dontusemixedcaseclassnames(std::vector<Token>::iterator* it,
                                      bool* dontusemixedcaseclassnames) {
  if ((*it)->type != TokenType::dontusemixedcaseclassnames_token) {
    return false;
  }
  *dontusemixedcaseclassnames = true;
  ++(*it);
  return true;
}

bool parse_dontpreverify(std::vector<Token>::iterator* it,
                         bool* dontpreverify) {
  if ((*it)->type != TokenType::dontpreverify_token) {
    return false;
  }
  *dontpreverify = true;
  ++(*it);
  return true;
}

bool parse_verbose(std::vector<Token>::iterator* it, bool* verbose) {
  if ((*it)->type != TokenType::verbose_token) {
    return false;
  }
  *verbose = true;
  ++(*it);
  return true;
}

bool parse_bool_command(std::vector<Token>::iterator* it,
                        TokenType bool_command_token,
                        bool new_value,
                        bool* bool_value) {
  if ((*it)->type == bool_command_token) {
    ++(*it); // Consume the boolean command token.
    *bool_value = new_value;
    return true;
  }
  return false;
}

bool parse_repackageclasses(std::vector<Token>::iterator* it) {
  if ((*it)->type != TokenType::repackageclasses) {
    return false;
  }
  // Ignore repackageclasses.
  ++(*it);
  if ((*it)->type == TokenType::identifier) {
    std::cerr << "Ignoring -repackageclasses " << (*it)->data << std::endl;
    ++(*it);
  }
  return true;
}

bool parse_target(std::vector<Token>::iterator* it,
                  std::string* target_version) {
  if ((*it)->type == TokenType::target) {
    ++(*it); // Consume the target command token.
    // Check to make sure the next TokenType is a version token.
    if ((*it)->type != TokenType::target_version_token) {
      std::cerr << "Expected a target version but got " << (*it)->show()
                << " at line " << (*it)->line << std::endl;
      return true;
    }
    *target_version = (*it)->data.to_string();
    // Consume the target version token.
    ++(*it);
    return true;
  }
  return false;
}

bool parse_allowaccessmodification(std::vector<Token>::iterator* it,
                                   bool* allowaccessmodification) {
  if ((*it)->type != TokenType::allowaccessmodification_token) {
    return false;
  }
  ++(*it);
  *allowaccessmodification = true;
  return true;
}

bool parse_filter_list_command(std::vector<Token>::iterator* it,
                               TokenType filter_command_token,
                               std::vector<std::string>* filters) {
  if ((*it)->type != filter_command_token) {
    return false;
  }
  ++(*it);
  while ((*it)->type == TokenType::filter_pattern) {
    filters->push_back((*it)->data.to_string());
    ++(*it);
  }
  return true;
}

bool parse_optimizationpasses_command(std::vector<Token>::iterator* it) {
  if ((*it)->type != TokenType::optimizationpasses) {
    return false;
  }
  ++(*it);
  // Comsume the next token.
  ++(*it);
  return true;
}

bool is_modifier(TokenType tok) {
  return tok == TokenType::includedescriptorclasses_token ||
         tok == TokenType::allowshrinking_token ||
         tok == TokenType::allowoptimization_token ||
         tok == TokenType::allowobfuscation_token;
}

bool parse_modifiers(std::vector<Token>::iterator* it, KeepSpec* keep) {
  while ((*it)->type == TokenType::comma) {
    ++(*it);
    if (!is_modifier((*it)->type)) {
      std::cerr << "Expected keep option modifier but found : " << (*it)->show()
                << " at line number " << (*it)->line << std::endl;
      return false;
    }
    switch ((*it)->type) {
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
    ++(*it);
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

std::string parse_annotation_type(std::vector<Token>::iterator* it) {
  if ((*it)->type != TokenType::annotation_application) {
    return "";
  }
  ++(*it);
  if ((*it)->type != TokenType::identifier) {
    std::cerr << "Expecting a class identifier after @ but got "
              << (*it)->show() << " at line " << (*it)->line << std::endl;
    return "";
  }
  const auto& typ = (*it)->data;
  ++(*it);
  return convert_wildcard_type(typ.to_string());
}

bool is_access_flag_set(const DexAccessFlags accessFlags,
                        const DexAccessFlags checkingFlag) {
  return accessFlags & checkingFlag;
}

void set_access_flag(DexAccessFlags& accessFlags,
                     const DexAccessFlags settingFlag) {
  accessFlags = accessFlags | settingFlag;
}

bool parse_access_flags(std::vector<Token>::iterator* it,
                        DexAccessFlags& setFlags_,
                        DexAccessFlags& unsetFlags_) {
  bool negated = false;
  while (is_negation_or_class_access_modifier((*it)->type)) {
    // Copy the iterator so we can peek and see if the next TokenType is an
    // access token; we don't want to modify the main iterator otherwise
    auto access_it = *it;
    if ((*it)->type == TokenType::notToken) {
      negated = true;
      ++access_it;
    }
    bool ok;
    DexAccessFlags access_flag = process_access_modifier(access_it->type, &ok);
    if (ok) {
      *it = ++access_it;
      if (negated) {
        if (is_access_flag_set(setFlags_, access_flag)) {
          std::cerr << "Access flag " << (*it)->show()
                    << " occurs with conflicting settings at line "
                    << (*it)->line << std::endl;
          return false;
        }
        set_access_flag(unsetFlags_, access_flag);
        negated = false;
      } else {
        if (is_access_flag_set(unsetFlags_, access_flag)) {
          std::cerr << "Access flag " << (*it)->show()
                    << " occurs with conflicting settings at line "
                    << (*it)->line << std::endl;
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
bool parse_class_token(std::vector<Token>::iterator* it,
                       DexAccessFlags& setFlags_,
                       DexAccessFlags& unsetFlags_) {
  bool negated = false;
  if ((*it)->type == TokenType::notToken) {
    negated = true;
    ++(*it);
  }
  // Make sure the next keyword is interface, class, enum.
  switch ((*it)->type) {
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
    std::cerr << "Expected interface, class or enum but got " << (*it)->show()
              << " at line number " << (*it)->line << std::endl;
    return false;
  }
  ++(*it);
  return true;
}

// Consume an expected token, indicating if that TokenType was found.
// If some other TokenType is found, then it is not consumed and false
// is returned.
bool consume_token(std::vector<Token>::iterator* it, const TokenType& tok) {
  if ((*it)->type != tok) {
    std::cerr << "Unexpected TokenType " << (*it)->show() << std::endl;
    return false;
  }
  ++(*it);
  return true;
}

// Consume an expected semicolon, complaining if one was not found.
void gobble_semicolon(std::vector<Token>::iterator* it, bool* ok) {
  *ok = consume_token(it, TokenType::semiColon);
  if (!*ok) {
    std::cerr << "Expecting a semicolon but found " << (*it)->show()
              << " at line " << (*it)->line << std::endl;
    return;
  }
}

void skip_to_semicolon(std::vector<Token>::iterator* it) {
  while (((*it)->type != TokenType::semiColon) &&
         ((*it)->type != TokenType::eof_token)) {
    ++(*it);
  }
  if ((*it)->type == TokenType::semiColon) {
    ++(*it);
  }
}

void parse_member_specification(std::vector<Token>::iterator* it,
                                ClassSpecification* class_spec,
                                bool* ok) {
  MemberSpecification member_specification;
  *ok = true;
  member_specification.annotationType = parse_annotation_type(it);
  if (!parse_access_flags(it,
                          member_specification.requiredSetAccessFlags,
                          member_specification.requiredUnsetAccessFlags)) {
    // There was a problem parsing the access flags. Return an empty class spec
    // for now.
    std::cerr << "Problem parsing access flags for member specification.\n";
    *ok = false;
    skip_to_semicolon(it);
    return;
  }
  // The next TokenType better be an identifier.
  if ((*it)->type != TokenType::identifier) {
    std::cerr << "Expecting field or member specification but got "
              << (*it)->show() << " at line " << (*it)->line << std::endl;
    *ok = false;
    skip_to_semicolon(it);
    return;
  }
  const auto& ident = (*it)->data;
  // Check for "*".
  if (ident == "*") {
    member_specification.name = "";
    member_specification.descriptor = "";
    ++(*it);
    gobble_semicolon(it, ok);
    class_spec->methodSpecifications.push_back(member_specification);
    class_spec->fieldSpecifications.push_back(member_specification);
    return;
  }
  // Check for <methods>
  if (ident == "<methods>") {
    member_specification.name = "";
    member_specification.descriptor = "";
    ++(*it);
    gobble_semicolon(it, ok);
    class_spec->methodSpecifications.push_back(member_specification);
    return;
  }
  // Check for <fields>
  if (ident == "<fields>") {
    member_specification.name = "";
    member_specification.descriptor = "";
    ++(*it);
    gobble_semicolon(it, ok);
    class_spec->fieldSpecifications.push_back(member_specification);
    return;
  }
  // Check for <init>
  if (ident == "<init>") {
    member_specification.name = "<init>";
    member_specification.descriptor = "V";
    set_access_flag(member_specification.requiredSetAccessFlags,
                    ACC_CONSTRUCTOR);
    ++(*it);
  } else {
    // This TokenType is the type for the member specification.
    if ((*it)->type != TokenType::identifier) {
      std::cerr << "Expecting type identifier but got " << (*it)->show()
                << " at line " << (*it)->line << std::endl;
      *ok = false;
      skip_to_semicolon(it);
      return;
    }
    const auto& typ = (*it)->data;
    ++(*it);
    member_specification.descriptor = convert_wildcard_type(typ.to_string());
    if ((*it)->type != TokenType::identifier) {
      std::cerr << "Expecting identifier name for class member but got "
                << (*it)->show() << " at line " << (*it)->line << std::endl;
      *ok = false;
      skip_to_semicolon(it);
      return;
    }
    member_specification.name = (*it)->data.to_string();
    ++(*it);
  }
  // Check to see if this is a method specification.
  if ((*it)->type == TokenType::openBracket) {
    consume_token(it, TokenType::openBracket);
    std::string arg = "(";
    while (true) {
      // If there is a ")" next we are done.
      if ((*it)->type == TokenType::closeBracket) {
        consume_token(it, TokenType::closeBracket);
        break;
      }
      if ((*it)->type != TokenType::identifier) {
        std::cerr << "Expecting type identifier but got " << (*it)->show()
                  << " at line " << (*it)->line << std::endl;
        *ok = false;
        return;
      }
      const auto& typ = (*it)->data;
      consume_token(it, TokenType::identifier);
      arg += convert_wildcard_type(typ.to_string());
      // The next TokenType better be a comma or a closing bracket.
      if ((*it)->type != TokenType::comma &&
          (*it)->type != TokenType::closeBracket) {
        std::cerr << "Expecting comma or ) but got " << (*it)->show()
                  << " at line " << (*it)->line << std::endl;
        *ok = false;
        return;
      }
      // If the next TokenType is a comma (rather than closing bracket) consume
      // it and check that it is followed by an identifier.
      if ((*it)->type == TokenType::comma) {
        consume_token(it, TokenType::comma);
        if ((*it)->type != TokenType::identifier) {
          std::cerr << "Expecting type identifier after comma but got "
                    << (*it)->show() << " at line " << (*it)->line << std::endl;
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
  if ((*it)->type == TokenType::returns) {
    ++(*it);
    const auto& rident = (*it)->data;
    if (rident == "true") {
      member_specification.return_value.value_type =
          AssumeReturnValue::ValueType::ValueBool;
      member_specification.return_value.value.b = true;
      ++(*it);
    }
    if (rident == "false") {
      member_specification.return_value.value_type =
          AssumeReturnValue::ValueType::ValueBool;
      member_specification.return_value.value.b = false;
      ++(*it);
    }
  }
  // Make sure member specification ends with a semicolon.
  gobble_semicolon(it, ok);
  if (!ok) {
    return;
  }
  if (member_specification.descriptor[0] == '(') {
    class_spec->methodSpecifications.push_back(member_specification);
  } else {
    class_spec->fieldSpecifications.push_back(member_specification);
  }
}

void parse_member_specifications(std::vector<Token>::iterator* it,
                                 ClassSpecification* class_spec,
                                 bool* ok) {
  if ((*it)->type == TokenType::openCurlyBracket) {
    ++(*it);
    while (((*it)->type != TokenType::closeCurlyBracket) &&
           ((*it)->type != TokenType::eof_token)) {
      parse_member_specification(it, class_spec, ok);
      if (!*ok) {
        // We failed to parse a member specification so skip to the next
        // semicolon.
        skip_to_semicolon(it);
      }
    }
    if ((*it)->type == TokenType::closeCurlyBracket) {
      ++(*it);
    }
  }
}

bool member_comparison(const MemberSpecification& m1,
                       const MemberSpecification& m2) {
  return m1.name < m2.name;
}

ClassSpecification parse_class_specification(std::vector<Token>::iterator* it,
                                             bool* ok) {
  ClassSpecification class_spec;
  *ok = true;
  class_spec.annotationType = parse_annotation_type(it);
  if (!parse_access_flags(
          it, class_spec.setAccessFlags, class_spec.unsetAccessFlags)) {
    // There was a problem parsing the access flags. Return an empty class spec
    // for now.
    std::cerr << "Problem parsing access flags for class specification.\n";
    *ok = false;
    return class_spec;
  }
  if (!parse_class_token(
          it, class_spec.setAccessFlags, class_spec.unsetAccessFlags)) {
    *ok = false;
    return class_spec;
  }
  // Parse the class name.
  if ((*it)->type != TokenType::identifier) {
    std::cerr << "Expected class name but got " << (*it)->show() << " at line "
              << (*it)->line << std::endl;
    *ok = false;
    return class_spec;
  }
  class_spec.className = (*it)->data.to_string();
  ++(*it);
  // Parse extends/implements if present, treating implements like extends.
  if (((*it)->type == TokenType::extends) ||
      ((*it)->type == TokenType::implements)) {
    ++(*it);
    class_spec.extendsAnnotationType = parse_annotation_type(it);
    if ((*it)->type != TokenType::identifier) {
      std::cerr << "Expecting a class name after extends/implements but got "
                << (*it)->show() << " at line " << (*it)->line << std::endl;
      *ok = false;
      class_spec.extendsClassName = "";
    } else {
      class_spec.extendsClassName = (*it)->data.to_string();
    }
    ++(*it);
  }
  // Parse the member specifications, if there are any
  parse_member_specifications(it, &class_spec, ok);
  std::sort(class_spec.fieldSpecifications.begin(),
            class_spec.fieldSpecifications.end(),
            member_comparison);
  std::sort(class_spec.methodSpecifications.begin(),
            class_spec.methodSpecifications.end(),
            member_comparison);
  return class_spec;
}

bool parse_keep(std::vector<Token>::iterator* it,
                TokenType keep_kind,
                KeepSpecSet* spec,
                bool mark_classes,
                bool mark_conditionally,
                bool allowshrinking,
                const std::string& filename,
                uint32_t line,
                bool* ok) {
  if ((*it)->type == keep_kind) {
    ++(*it); // Consume the keep token
    auto keep = std::make_unique<KeepSpec>();
    keep->mark_classes = mark_classes;
    keep->mark_conditionally = mark_conditionally;
    keep->allowshrinking = allowshrinking;
    keep->source_filename = filename;
    keep->source_line = line;
    if (!parse_modifiers(it, &*keep)) {
      skip_to_next_command(it);
      return true;
    }
    keep->class_spec = parse_class_specification(it, ok);
    spec->emplace(std::move(keep));
    return true;
  }
  return false;
}

void parse(std::vector<Token>::iterator it,
           std::vector<Token>::iterator tokens_end,
           ProguardConfiguration* pg_config,
           unsigned int* parse_errors,
           const std::string& filename) {
  *parse_errors = 0;
  bool ok;
  while (it != tokens_end) {
    // Break out if we are at the end of the TokenType stream.
    if (it->type == TokenType::eof_token) {
      break;
    }
    uint32_t line = it->line;
    if (!it->is_command()) {
      std::cerr << "Expecting command but found " << it->show() << " at line "
                << it->line << std::endl;
      ++it;
      skip_to_next_command(&it);
      continue;
    }

    // Input/Output Options
    if (parse_filepath_command(&it,
                               TokenType::include,
                               pg_config->basedirectory,
                               &pg_config->includes)) {
      continue;
    }
    if (parse_single_filepath_command(
            &it, TokenType::basedirectory, &pg_config->basedirectory)) {
      continue;
    }
    if (parse_jars(&it,
                   TokenType::injars,
                   pg_config->basedirectory,
                   &pg_config->injars)) {
      continue;
    }
    if (parse_jars(&it,
                   TokenType::outjars,
                   pg_config->basedirectory,
                   &pg_config->outjars)) {
      continue;
    }
    if (parse_jars(&it,
                   TokenType::libraryjars,
                   pg_config->basedirectory,
                   &pg_config->libraryjars)) {
      continue;
    }
    // -skipnonpubliclibraryclasses not supported
    if (it->type == TokenType::dontskipnonpubliclibraryclasses) {
      // Silenty ignore the dontskipnonpubliclibraryclasses option.
      ++it;
      continue;
    }
    // -dontskipnonpubliclibraryclassmembers not supported
    if (parse_filepath_command(&it,
                               TokenType::keepdirectories,
                               pg_config->basedirectory,
                               &pg_config->keepdirectories)) {
      continue;
    }
    if (parse_target(&it, &pg_config->target_version)) {
      continue;
    }
    // -forceprocessing not supported

    // Keep Options
    if (parse_keep(&it,
                   TokenType::keep,
                   &pg_config->keep_rules,
                   true, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        (*parse_errors)++;
      }
      continue;
    }
    if (parse_keep(&it,
                   TokenType::keepclassmembers,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        (*parse_errors)++;
      }
      continue;
    }
    if (parse_keep(&it,
                   TokenType::keepclasseswithmembers,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   true, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        (*parse_errors)++;
      }
      continue;
    }
    if (parse_keep(&it,
                   TokenType::keepnames,
                   &pg_config->keep_rules,
                   true, // mark_classes
                   false, // mark_conditionally
                   true, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        (*parse_errors)++;
      }
      continue;
    }
    if (parse_keep(&it,
                   TokenType::keepclassmembernames,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   true, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        (*parse_errors)++;
      }
      continue;
    }
    if (parse_keep(&it,
                   TokenType::keepclasseswithmembernames,
                   &pg_config->keep_rules,
                   false, // mark_classes
                   true, // mark_conditionally
                   true, // allowshrinking
                   filename,
                   line,
                   &ok)) {
      if (!ok) {
        (*parse_errors)++;
      }
      continue;
    }
    if (parse_optional_filepath_command(
            &it, TokenType::printseeds, &pg_config->printseeds)) {
      continue;
    }

    // Shrinking Options
    if (parse_bool_command(
            &it, TokenType::dontshrink, false, &pg_config->shrink)) {
      continue;
    }
    if (parse_optional_filepath_command(
            &it, TokenType::printusage, &pg_config->printusage)) {
      continue;
    }

    // Optimization Options
    if (parse_boolean_command(
            &it, TokenType::dontoptimize, &pg_config->optimize, false)) {
      continue;
    }
    if (parse_filter_list_command(
            &it, TokenType::optimizations, &pg_config->optimization_filters)) {
      continue;
    }
    if (parse_optimizationpasses_command(&it)) {
      continue;
    }
    if (parse_keep(&it,
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
    if (parse_keep(&it,
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
    if (it->type == TokenType::dontobfuscate) {
      pg_config->dontobfuscate = true;
      ++it;
      continue;
    }
    // Redex ignores -dontskipnonpubliclibraryclasses
    if (it->type == TokenType::dontskipnonpubliclibraryclasses) {
      ++it;
      continue;
    }
    if (parse_optional_filepath_command(
            &it, TokenType::printmapping, &pg_config->printmapping)) {
      continue;
    }
    if (parse_optional_filepath_command(&it,
                                        TokenType::printconfiguration,
                                        &pg_config->printconfiguration)) {
      continue;
    }

    if (parse_allowaccessmodification(&it,
                                      &pg_config->allowaccessmodification)) {
      continue;
    }
    if (parse_dontusemixedcaseclassnames(
            &it, &pg_config->dontusemixedcaseclassnames)) {
      continue;
    }
    if (parse_filter_list_command(
            &it, TokenType::keeppackagenames, &pg_config->keeppackagenames)) {
      continue;
    }
    if (parse_dontpreverify(&it, &pg_config->dontpreverify)) {
      continue;
    }
    if (parse_verbose(&it, &pg_config->verbose)) {
      continue;
    }
    if (parse_repackageclasses(&it)) {
      continue;
    }

    if (parse_filter_list_command(
            &it, TokenType::dontwarn, &pg_config->dontwarn)) {
      continue;
    }
    if (parse_filter_list_command(
            &it, TokenType::keepattributes, &pg_config->keepattributes)) {
      continue;
    }

    // Skip unknown token.
    if (it->is_command()) {
      const auto& name = it->data;
      // It is benign to drop -dontnote
      if (name != "dontnote") {
        std::cerr << "Unimplemented command (skipping): " << it->show()
                  << " at line " << it->line << std::endl;
      }
    } else {
      std::cerr << "Unexpected TokenType " << it->show() << " at line "
                << it->line << std::endl;
      (*parse_errors)++;
    }
    ++it;
    skip_to_next_command(&it);
  }
}

void parse(const boost::string_view& config,
           ProguardConfiguration* pg_config,
           const std::string& filename) {
  std::vector<Token> tokens = lex(config);
  bool ok = true;
  // Check for bad tokens.
  for (auto& tok : tokens) {
    if (tok.type == TokenType::unknownToken) {
      std::string spelling ATTRIBUTE_UNUSED = tok.data.to_string();
      ok = false;
    }
    // std::cout << tok->show() << " at line " << tok->line << std::endl;
  }
  unsigned int parse_errors = 0;
  if (ok) {
    parse(tokens.begin(), tokens.end(), pg_config, &parse_errors, filename);
  }

  if (parse_errors == 0) {
    pg_config->ok = ok;
  } else {
    pg_config->ok = false;
    std::cerr << "Found " << parse_errors << " parse errors\n";
  }
}

} // namespace

void parse(std::istream& config,
           ProguardConfiguration* pg_config,
           const std::string& filename) {
  std::stringstream buffer;
  buffer << config.rdbuf();
  parse(buffer.str(), pg_config, filename);
}

void parse_file(const std::string& filename, ProguardConfiguration* pg_config) {
  redex::read_file_with_contents(filename, [&](const char* data, size_t s) {
    boost::string_view view(data, s);
    parse(view, pg_config, filename);
    // Parse the included files.
    for (const auto& included_filename : pg_config->includes) {
      if (pg_config->already_included.find(included_filename) !=
          pg_config->already_included.end()) {
        continue;
      }
      pg_config->already_included.emplace(included_filename);
      parse_file(included_filename, pg_config);
    }
  });
}

void remove_blocklisted_rules(ProguardConfiguration* pg_config) {
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
  // std::stringstream ss(blocklisted_rules);
  ProguardConfiguration pg_config_blocklist;
  parse(blocklisted_rules, &pg_config_blocklist, "<internal blocklist>");
  pg_config->keep_rules.erase_if([&](const KeepSpec& ks) {
    for (const auto& blocklisted_ks : pg_config_blocklist.keep_rules) {
      if (ks == *blocklisted_ks) {
        return true;
      }
    }
    return false;
  });
}

} // namespace proguard_parser
} // namespace keep_rules
