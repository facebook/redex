/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <fstream>
#include <iostream>

#include "ProguardLexer.h"
#include "ProguardMap.h"
#include "ProguardParser.h"
#include "ProguardRegex.h"

namespace redex {
namespace proguard_parser {

bool parse_boolean_command(std::vector<unique_ptr<Token>>::iterator* it,
                           token boolean_option,
                           bool* option,
                           bool value) {
  if ((**it)->type != boolean_option) {
    return false;
  }
  ++(*it);
  *option = value;
  return true;
}

void skip_to_next_command(std::vector<unique_ptr<Token>>::iterator* it) {
  while (((**it)->type != token::eof_token) && (!(**it)->is_command())) {
    ++(*it);
  }
}

bool parse_single_filepath_command(std::vector<unique_ptr<Token>>::iterator* it,
                                   token filepath_command_token,
                                   std::string* filepath) {
  if ((**it)->type == filepath_command_token) {
    unsigned int line_number = (**it)->line;
    ++(*it); // Consume the command token.
    // Fail without consumption if this is an end of file token.
    if ((**it)->type == token::eof_token) {
      cerr << "Expecting at least one file as an argument but found end of "
              "file at line "
           << line_number << endl;
      return true;
    }
    // Fail without consumption if this is a command token.
    if ((**it)->is_command()) {
      cerr << "Expecting a file path argument but got command "
           << (**it)->show() << " at line  " << (**it)->line << endl;
      return true;
    }
    // Parse the filename.
    if ((**it)->type != token::filepath) {
      cerr << "Expected a filepath but got " << (**it)->show() << " at line "
           << (**it)->line << endl;
      return true;
    }
    *filepath = static_cast<Filepath*>((*it)->get())->path;
    ++(*it); // Consume the filepath token
    return true;
  }
  return false;
}

std::vector<std::string> parse_filepaths(
    std::vector<unique_ptr<Token>>::iterator* it) {
  std::vector<std::string> filepaths;
  if ((**it)->type != token::filepath) {
    cerr << "Expected filepath but got " << (**it)->show() << " at line "
         << (**it)->line << endl;
    return filepaths;
  }
  while ((**it)->type == token::filepath) {
    filepaths.push_back(static_cast<Filepath*>((*it)->get())->path);
    ++(*it);
  }
  return filepaths;
}

bool parse_filepath_command(std::vector<unique_ptr<Token>>::iterator* it,
                            token filepath_command_token,
                            const std::string& basedir,
                            std::vector<std::string>* filepaths) {
  if ((**it)->type == filepath_command_token) {
    unsigned int line_number = (**it)->line;
    ++(*it); // Consume the command token.
    // Fail without consumption if this is an end of file token.
    if ((**it)->type == token::eof_token) {
      cerr << "Expecting at least one file as an argument but found end of "
              "file at line "
           << line_number << endl;
      return true;
    }
    // Fail without consumption if this is a command token.
    if ((**it)->is_command()) {
      cerr << "Expecting a file path argument but got command "
           << (**it)->show() << " at line  " << (**it)->line << endl;
      return true;
    }
    // Parse the filename.
    if ((**it)->type != token::filepath) {
      cerr << "Expected a filepath but got " << (**it)->show() << " at line "
           << (**it)->line << endl;
      return true;
    }
    for (const auto& filepath : parse_filepaths(it)) {
      filepaths->push_back(filepath);
    }
    return true;
  }
  return false;
}

bool parse_optional_filepath_command(
    std::vector<unique_ptr<Token>>::iterator* it,
    token filepath_command_token,
    std::vector<std::string>* filepaths) {
  if ((**it)->type != filepath_command_token) {
    return false;
  }
  ++(*it); // Consume the command token.
  // Parse an optional filepath argument.
  if ((**it)->type == token::filepath) {
    filepaths->push_back(static_cast<Filepath*>((*it)->get())->path);
    ++(*it);
  }
  return true;
}

bool parse_jars(std::vector<unique_ptr<Token>>::iterator* it,
                token jar_token,
                const std::string& basedir,
                std::vector<std::string>* jars) {
  if ((**it)->type == jar_token) {
    unsigned int line_number = (**it)->line;
    ++(*it); // Consume the jar token.
    // Fail without consumption if this is an end of file token.
    if ((**it)->type == token::eof_token) {
      cerr << "Expecting at least one file as an argument but found end of "
              "file at line "
           << line_number << endl;
      return true;
    }
    // Parse the list of filenames.
    for (const auto& filepath : parse_filepaths(it)) {
      jars->push_back(filepath);
    }
    return true;
  }
  return false;
}

bool parse_dontusemixedcaseclassnames(
    std::vector<unique_ptr<Token>>::iterator* it,
    bool* dontusemixedcaseclassnames) {
  if ((**it)->type != token::dontusemixedcaseclassnames_token) {
    return false;
  }
  *dontusemixedcaseclassnames = true;
  ++(*it);
  return true;
}

bool parse_dontpreverify(std::vector<unique_ptr<Token>>::iterator* it,
                         bool* dontpreverify) {
  if ((**it)->type != token::dontpreverify_token) {
    return false;
  }
  *dontpreverify = true;
  ++(*it);
  return true;
}

bool parse_verbose(std::vector<unique_ptr<Token>>::iterator* it,
                   bool* verbose) {
  if ((**it)->type != token::verbose_token) {
    return false;
  }
  *verbose = true;
  ++(*it);
  return true;
}

bool parse_bool_command(std::vector<unique_ptr<Token>>::iterator* it,
                        token bool_command_token,
                        bool new_value,
                        bool* bool_value) {
  if ((**it)->type == bool_command_token) {
    ++(*it); // Consume the boolean command token.
    *bool_value = new_value;
    return true;
  }
  return false;
}

bool parse_repackageclasses(std::vector<unique_ptr<Token>>::iterator* it) {
  if ((**it)->type != token::repackageclasses) {
    return false;
  }
  // Ignore repackageclasses.
  ++(*it);
  if ((**it)->type == token::identifier) {
    cerr << "Ignoring -repackageclasses "
         << static_cast<Identifier*>((*it)->get())->ident << endl;
    ++(*it);
  }
  return true;
}

bool parse_target(std::vector<unique_ptr<Token>>::iterator* it,
                  std::string* target_version) {
  if ((**it)->type == token::target) {
    ++(*it); // Consume the target command token.
    // Check to make sure the next token is a version token.
    if ((**it)->type != token::target_version_token) {
      cerr << "Expected a target version but got " << (**it)->show()
           << " at line " << (**it)->line << endl;
      return true;
    }
    *target_version = static_cast<TargetVersion*>((*it)->get())->target_version;
    // Consume the target version token.
    ++(*it);
    return true;
  }
  return false;
}

bool parse_allowaccessmodification(std::vector<unique_ptr<Token>>::iterator* it,
                                   bool* allowaccessmodification) {
  if ((**it)->type != token::allowaccessmodification_token) {
    return false;
  }
  ++(*it);
  *allowaccessmodification = true;
  return true;
}

bool parse_filter_list_command(std::vector<unique_ptr<Token>>::iterator* it,
                               token filter_command_token,
                               std::vector<std::string>* filters) {
  if ((**it)->type != filter_command_token) {
    return false;
  }
  ++(*it);
  while ((**it)->type == token::filter_pattern) {
    filters->push_back(static_cast<Filter*>((*it)->get())->filter);
    ++(*it);
  }
  return true;
}

bool parse_optimizationpasses_command(
    std::vector<unique_ptr<Token>>::iterator* it) {
  if ((**it)->type != token::optimizationpasses) {
    return false;
  }
  ++(*it);
  // Comsume the next token.
  ++(*it);
  return true;
}

bool is_modifier(token tok) {
  return tok == token::includedescriptorclasses_token ||
         tok == token::allowshrinking_token ||
         tok == token::allowoptimization_token ||
         tok == token::allowobfuscation_token;
}

bool parse_modifiers(std::vector<unique_ptr<Token>>::iterator* it,
                     KeepSpec* keep) {
  while ((**it)->type == token::comma) {
    ++(*it);
    if (!is_modifier((**it)->type)) {
      cerr << "Expected keep option modifier but found : " << (**it)->show()
           << " at line number " << (**it)->line << endl;
      return false;
    }
    switch ((**it)->type) {
    case token::includedescriptorclasses_token:
      keep->includedescriptorclasses = true;
      break;
    case token::allowshrinking_token:
      keep->allowshrinking = true;
      break;
    case token::allowoptimization_token:
      keep->allowoptimization = true;
      break;
    case token::allowobfuscation_token:
      keep->allowobfuscation = true;
      break;
    default:
      break;
    }
    ++(*it);
  }
  return true;
}

DexAccessFlags process_access_modifier(token type, bool* is_access_flag) {
  *is_access_flag = true;
  switch (type) {
  case token::publicToken:
    return ACC_PUBLIC;
  case token::privateToken:
    return ACC_PRIVATE;
  case token::final:
    return ACC_FINAL;
  case token::interface:
    return ACC_INTERFACE;
  case token::abstract:
    return ACC_ABSTRACT;
  case token::synthetic:
    return ACC_SYNTHETIC;
  case token::annotation:
    return ACC_ANNOTATION;
  case token::enumToken:
    return ACC_ENUM;
  case token::staticToken:
    return ACC_STATIC;
  case token::native:
    return ACC_NATIVE;
  case token::protectedToken:
    return ACC_PROTECTED;
  case token::transient:
    return ACC_TRANSIENT;
  default:
    *is_access_flag = false;
    return ACC_PUBLIC;
  }
}

bool is_negation_or_class_access_modifier(token type) {
  switch (type) {
  case token::notToken:
  case token::publicToken:
  case token::privateToken:
  case token::protectedToken:
  case token::final:
  case token::abstract:
  case token::synthetic:
  case token::native:
  case token::staticToken:
  case token::transient:
  case token::annotation:
    return true;
  default:
    return false;
  }
}

std::string parse_annotation_type(
    std::vector<unique_ptr<Token>>::iterator* it) {
  if ((**it)->type != token::annotation_application) {
    return "";
  }
  ++(*it);
  if ((**it)->type != token::identifier) {
    cerr << "Expecting a class identifier after @ but got " << (**it)->show()
         << " at line " << (**it)->line << endl;
    return "";
  }
  auto typ = static_cast<Identifier*>((*it)->get())->ident;
  ++(*it);
  return convert_wildcard_type(typ);
}

bool is_access_flag_set(const DexAccessFlags accessFlags,
                        const DexAccessFlags checkingFlag) {
  return accessFlags & checkingFlag;
}

void set_access_flag(DexAccessFlags& accessFlags,
                     const DexAccessFlags settingFlag) {
  accessFlags = accessFlags | settingFlag;
  return;
}

bool parse_access_flags(std::vector<unique_ptr<Token>>::iterator* it,
                        DexAccessFlags& setFlags_,
                        DexAccessFlags& unsetFlags_) {
  bool negated = false;
  while (is_negation_or_class_access_modifier((**it)->type)) {
    // Consume the negation token if present.
    if ((**it)->type == token::notToken) {
      negated = true;
      ++(*it);
      continue;
    }
    bool ok;
    DexAccessFlags access_flag = process_access_modifier((**it)->type, &ok);
    if (ok) {
      ++(*it);
      if (negated) {
        if (is_access_flag_set(setFlags_, access_flag)) {
          cerr << "Access flag " << (**it)->show()
               << " occurs with conflicting settings at line " << (**it)->line
               << endl;
          return false;
        }
        set_access_flag(unsetFlags_, access_flag);
        negated = false;
      } else {
        if (is_access_flag_set(unsetFlags_, access_flag)) {
          cerr << "Access flag " << (**it)->show()
               << " occurs with conflicting settings at line " << (**it)->line
               << endl;
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

// Consume an expected token, indicating if that token was found.
// If some other token is found, then it is not consumed and false
// is returned.
bool consume_token(std::vector<unique_ptr<Token>>::iterator* it,
                   const token& tok) {
  if ((**it)->type != tok) {
    cerr << "Unexpected token " << (**it)->show() << std::endl;
    return false;
  }
  ++(*it);
  return true;
}

// Consume an expected semicolon, complaining if one was not found.
void gobble_semicolon(std::vector<unique_ptr<Token>>::iterator* it, bool* ok) {
  *ok = consume_token(it, token::semiColon);
  if (!*ok) {
    cerr << "Expecting a semicolon but found " << (**it)->show() << " at line "
         << (**it)->line << std::endl;
    return;
  }
}

void skip_to_semicolon(std::vector<unique_ptr<Token>>::iterator* it) {
  while (((**it)->type != token::semiColon) &&
         ((**it)->type != token::eof_token)) {
    ++(*it);
  }
  if ((**it)->type == token::semiColon) {
    ++(*it);
  }
}

void parse_member_specification(std::vector<unique_ptr<Token>>::iterator* it,
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
    cerr << "Problem parsing access flags for member specification.\n";
    *ok = false;
    skip_to_semicolon(it);
    return;
  }
  // The next token better be an identifier.
  if ((**it)->type != token::identifier) {
    cerr << "Expecting field or member specification but got " << (**it)->show()
         << " at line " << (**it)->line << endl;
    *ok = false;
    skip_to_semicolon(it);
    return;
  }
  std::string ident = static_cast<Identifier*>((*it)->get())->ident;
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
    // This token is the type for the member specification.
    if ((**it)->type != token::identifier) {
      cerr << "Expecting type identifier but got " << (**it)->show()
           << " at line " << (**it)->line << endl;
      *ok = false;
      skip_to_semicolon(it);
      return;
    }
    std::string typ = static_cast<Identifier*>((*it)->get())->ident;
    ++(*it);
    member_specification.descriptor = convert_wildcard_type(typ);
    if ((**it)->type != token::identifier) {
      cerr << "Expecting identifier name for class member but got "
           << (**it)->show() << " at line " << (**it)->line << endl;
      *ok = false;
      skip_to_semicolon(it);
      return;
    }
    member_specification.name = static_cast<Identifier*>((*it)->get())->ident;
    ++(*it);
  }
  // Check to see if this is a method specification.
  if ((**it)->type == token::openBracket) {
    consume_token(it, token::openBracket);
    std::string arg = "(";
    while (true) {
      // If there is a ")" next we are done.
      if ((**it)->type == token::closeBracket) {
        consume_token(it, token::closeBracket);
        break;
      }
      if ((**it)->type != token::identifier) {
        std::cerr << "Expecting type identifier but got " << (**it)->show()
                  << " at line " << (**it)->line << std::endl;
        *ok = false;
        return;
      }
      std::string typ = static_cast<Identifier*>((*it)->get())->ident;
      consume_token(it, token::identifier);
      arg += convert_wildcard_type(typ);
      // The next token better be a comma or a closing bracket.
      if ((**it)->type != token::comma && (**it)->type != token::closeBracket) {
        std::cerr << "Expecting comma or ) but got " << (**it)->show()
                  << " at line " << (**it)->line << std::endl;
        *ok = false;
        return;
      }
      // If the next token is a comma (rather than closing bracket) consume
      // it and check that it is followed by an identifier.
      if ((**it)->type == token::comma) {
        consume_token(it, token::comma);
        if ((**it)->type != token::identifier) {
          std::cerr << "Expecting type identifier after comma but got "
                    << (**it)->show() << " at line " << (**it)->line
                    << std::endl;
          *ok = false;
          return;
        }
      }
    }
    arg += ")";
    arg += member_specification.descriptor;
    member_specification.descriptor = arg;
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
  return;
}

void parse_member_specifications(std::vector<unique_ptr<Token>>::iterator* it,
                                 ClassSpecification* class_spec,
                                 bool* ok) {
  if ((**it)->type == token::openCurlyBracket) {
    ++(*it);
    while (((**it)->type != token::closeCurlyBracket) &&
           ((**it)->type != token::eof_token)) {
      parse_member_specification(it, class_spec, ok);
      if (!*ok) {
        // We failed to parse a member specification so skip to the next
        // semicolon.
        skip_to_semicolon(it);
      }
    }
    if ((**it)->type == token::closeCurlyBracket) {
      ++(*it);
    }
  }
}

bool member_comparison(const MemberSpecification& m1,
                       const MemberSpecification& m2) {
  return m1.name < m2.name;
}

ClassSpecification parse_class_specification(
    std::vector<unique_ptr<Token>>::iterator* it, bool* ok) {
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
  // According to the ProGuard grammer the next token could be a '!' to express
  // a rule that
  // says !class or !interface or !enum. We choose to not implement this
  // feature.
  if ((**it)->type == token::notToken) {
    cerr << "Keep rules that match the negation of class, interface or enum "
            "are not supported.\n";
    *ok = false;
    return class_spec;
  }
  bool match_annotation_class = is_annotation(class_spec.setAccessFlags);
  if (!match_annotation_class) {
    // Make sure the next keyword is interface, class, enum.
    if (!(((**it)->type == token::interface) ||
          ((**it)->type == token::classToken) ||
          ((**it)->type == token::enumToken ||
           ((**it)->type == token::annotation)))) {
      cerr << "Expected interface, class or enum but got " << (**it)->show()
           << " at line number " << (**it)->line << endl;
      *ok = false;
      return class_spec;
    }
    // Restrict matches to interface classes
    if ((**it)->type == token::interface) {
      set_access_flag(class_spec.setAccessFlags, ACC_INTERFACE);
    }
    // Restrict matches to enum classes
    if ((**it)->type == token::enumToken) {
      set_access_flag(class_spec.setAccessFlags, ACC_ENUM);
    }
    ++(*it);
  }
  // Parse the class name.
  if ((**it)->type != token::identifier) {
    cerr << "Expected class name but got " << (**it)->show() << " at line "
         << (**it)->line << endl;
    *ok = false;
    return class_spec;
  }
  class_spec.className = static_cast<Identifier*>((*it)->get())->ident;
  ++(*it);
  // Parse extends/implements if present, treating implements like extends.
  if (((**it)->type == token::extends) || ((**it)->type == token::implements)) {
    ++(*it);
    class_spec.extendsAnnotationType = parse_annotation_type(it);
    if ((**it)->type != token::identifier) {
      cerr << "Expecting a class name after extends/implements but got "
           << (**it)->show() << " at line " << (**it)->line << endl;
      *ok = false;
      class_spec.extendsClassName = "";
    } else {
      class_spec.extendsClassName =
          static_cast<Identifier*>((*it)->get())->ident;
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

bool parse_keep(std::vector<unique_ptr<Token>>::iterator* it,
                token keep_kind,
                std::vector<KeepSpec>* spec,
                bool mark_classes,
                bool mark_conditionally,
                bool allowshrinking,
                const std::string& filename,
                uint32_t line,
                bool* ok) {
  if ((**it)->type == keep_kind) {
    ++(*it); // Consume the keep token
    KeepSpec keep;
    keep.mark_classes = mark_classes;
    keep.mark_conditionally = mark_conditionally;
    keep.allowshrinking = allowshrinking;
    keep.source_filename = filename;
    keep.source_line = line;
    if (!parse_modifiers(it, &keep)) {
      skip_to_next_command(it);
      return true;
    }
    keep.class_spec = parse_class_specification(it, ok);
    spec->push_back(keep);
    return true;
  }
  return false;
}

bool ignore_class_specification_command(
    std::vector<unique_ptr<Token>>::iterator* it, token classspec_command) {
  if ((**it)->type != classspec_command) {
    return false;
  }
  ++(*it);
  // Ignore the rest of the unsupported comamnd.
  skip_to_next_command(it);
  return true;
}

void parse(std::vector<unique_ptr<Token>>::iterator it,
           std::vector<unique_ptr<Token>>::iterator tokens_end,
           ProguardConfiguration* pg_config,
           unsigned int* parse_errors,
           const std::string& filename) {
  *parse_errors = 0;
  bool ok;
  while (it != tokens_end) {
    // Break out if we are at the end of the token stream.
    if ((*it)->type == token::eof_token) {
      break;
    }
    uint32_t line = (*it)->line;
    if (!(*it)->is_command()) {
      cerr << "Expecting command but found " << (*it)->show() << " at line "
           << (*it)->line << endl;
      ++it;
      skip_to_next_command(&it);
      continue;
    }

    // Input/Output Options
    if (parse_filepath_command(&it,
                               token::include,
                               pg_config->basedirectory,
                               &pg_config->includes))
      continue;
    if (parse_single_filepath_command(
            &it, token::basedirectory, &pg_config->basedirectory))
      continue;
    if (parse_jars(
            &it, token::injars, pg_config->basedirectory, &pg_config->injars))
      continue;
    if (parse_jars(
            &it, token::outjars, pg_config->basedirectory, &pg_config->outjars))
      continue;
    if (parse_jars(&it,
                   token::libraryjars,
                   pg_config->basedirectory,
                   &pg_config->libraryjars))
      continue;
    // -skipnonpubliclibraryclasses not supported
    if ((*it)->type == token::dontskipnonpubliclibraryclasses) {
      // Silenty ignore the dontskipnonpubliclibraryclasses option.
      ++it;
      continue;
    }
    // -dontskipnonpubliclibraryclassmembers not supported
    if (parse_filepath_command(&it,
                               token::keepdirectories,
                               pg_config->basedirectory,
                               &pg_config->keepdirectories))
      continue;
    if (parse_target(&it, &pg_config->target_version)) continue;
    // -forceprocessing not supported

    // Keep Options
    if (parse_keep(&it,
                   token::keep,
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
                   token::keepclassmembers,
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
                   token::keepclasseswithmembers,
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
                   token::keepnames,
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
                   token::keepclassmembernames,
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
                   token::keepclasseswithmembernames,
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
            &it, token::printseeds, &pg_config->printseeds))
      continue;

    // Shrinking Options
    if (parse_bool_command(&it, token::dontshrink, false, &pg_config->shrink))
      continue;
    if (parse_optional_filepath_command(
            &it, token::printusage, &pg_config->printusage))
      continue;

    // Optimization Options
    if (parse_boolean_command(
            &it, token::dontoptimize, &pg_config->optimize, false))
      continue;
    if (parse_filter_list_command(
            &it, token::optimizations, &pg_config->optimization_filters))
      continue;
    if (parse_optimizationpasses_command(&it)) {
      continue;
    }
    if (parse_keep(&it,
                   token::assumenosideeffects,
                   &pg_config->assumenosideeffects_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok))
      continue;
    if (parse_keep(&it,
                   token::whyareyoukeeping,
                   &pg_config->whyareyoukeeping_rules,
                   false, // mark_classes
                   false, // mark_conditionally
                   false, // allowshrinking
                   filename,
                   line,
                   &ok))
      continue;

    // Obfuscation Options
    if ((*it)->type == token::dontobfuscate) {
      pg_config->dontobfuscate = true;
      ++it;
      continue;
    }
    // Redex ignores -dontskipnonpubliclibraryclasses
    if ((*it)->type == token::dontskipnonpubliclibraryclasses) {
      ++it;
      continue;
    }
    if (parse_optional_filepath_command(
            &it, token::printmapping, &pg_config->printmapping))
      continue;
    if (parse_optional_filepath_command(
            &it, token::printconfiguration, &pg_config->printconfiguration))
      continue;

    if (parse_allowaccessmodification(&it, &pg_config->allowaccessmodification))
      continue;
    if (parse_dontusemixedcaseclassnames(
            &it, &pg_config->dontusemixedcaseclassnames))
      continue;
    if (parse_filter_list_command(
            &it, token::keeppackagenames, &pg_config->keeppackagenames))
      continue;
    if (parse_dontpreverify(&it, &pg_config->dontpreverify)) continue;
    if (parse_verbose(&it, &pg_config->verbose)) continue;
    if (parse_repackageclasses(&it)) continue;

    if (parse_filter_list_command(&it, token::dontwarn, &pg_config->dontwarn))
      continue;
    if (parse_filter_list_command(
            &it, token::keepattributes, &pg_config->keepattributes))
      continue;

    // Skip unknown token.
    if ((*it)->is_command()) {
      cerr << "Unimplemented command (skipping): " << (*it)->show()
           << " at line " << (*it)->line << endl;
    } else {
      cerr << "Unexpected token " << (*it)->show() << " at line " << (*it)->line
           << endl;
      (*parse_errors)++;
    }
    ++it;
    skip_to_next_command(&it);
  }
}

void parse(istream& config,
           ProguardConfiguration* pg_config,
           const std::string& filename) {
  std::vector<unique_ptr<Token>> tokens = lex(config);
  bool ok = true;
  // Check for bad tokens.
  for (auto& tok : tokens) {
    if (tok->type == token::unknownToken) {
      std::string spelling =
          static_cast<UnknownToken*>(tok.get())->token_string;
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
    cerr << "Found " << parse_errors << " parse errors\n";
  }
}

void parse_file(const std::string& filename, ProguardConfiguration* pg_config) {
  ifstream config(filename);
  // First try relative path.
  if (!config.is_open()) {
    // Try with -basedirectory
    config.open(pg_config->basedirectory + "/" + filename);
    if (!config.is_open()) {
      cerr << "ERROR: Failed to open ProGuard configuration file " << filename
           << endl;
      exit(1);
    }
  }

  parse(config, pg_config, filename);
  // Parse the included files.
  for (const auto& included_filename : pg_config->includes) {
    if (pg_config->already_included.find(included_filename) !=
        pg_config->already_included.end()) {
      continue;
    }
    pg_config->already_included.emplace(included_filename);
    parse_file(included_filename, pg_config);
  }
}

std::string show_bool(bool b) {
  if (b) {
    return "true";
  } else {
    return "false";
  }
}

} // namespace proguard_parser
} // namespace redex
