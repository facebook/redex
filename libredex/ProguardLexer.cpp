/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ProguardLexer.h"

namespace redex {
namespace proguard_parser {

bool is_deliminator(char ch) {
  return isspace(ch) || ch == '{' || ch == '}' || ch == '(' || ch == ')'
    || ch == ',' || ch == '[' || ch == ']'
    || ch == ';' || ch == ':' || ch == EOF;
}

// An identifier can refer to a class name, a field name or a package name.
bool is_identifier_character(char ch) {
  return isalnum(ch) || ch == '_' || ch == '$' || ch == '*' || ch == '.'
    || ch == '\'' || ch == '[' || ch == ']' || ch == '<' || ch == '>'
    || ch == '!' || ch == '?' || ch == '%';
}

bool is_identifier(std::string ident) {
  for (const char& ch : ident) {
    if (!is_identifier_character(ch)) {
      return false;
    }
  }
  return true;
}

void skip_whitespace(std::istream& config, unsigned int* line) {
  char ch;
  while (isspace(config.peek())) {
    config >> std::noskipws >> ch;
    if (ch == '\n') {
      ++(*line);
    }
  }
}

std::string read_path(std::istream& config, unsigned int* line) {
  std::string path;
  skip_whitespace(config, line);
  // Handle the case for optional filepath arguments by
  // returning an empty filepath.
  if (config.peek() == '-' || config.peek() == EOF) {
    return "";
  }
  while (config.peek() != ':' && !isspace(config.peek())
         && config.peek() != EOF) {
    char ch;
    config >> std::noskipws >> ch;
    if (ch == '"') {
      continue;
    }
    path += ch;
  }
  return path;
}

std::vector<std::string> read_paths(std::istream& config, unsigned int* line) {
  std::vector<std::string> paths;
  paths.push_back(read_path(config, line));
  while (config.peek() == ':') {
    char ch;
    config >> ch;
    paths.push_back(read_path(config, line));
  }
  return paths;
}

bool is_version_character(char ch) {
  return ch == '.' || isdigit(ch);
}

std::string read_target_version(std::istream& config, unsigned int* line) {
  std::string version;
  skip_whitespace(config, line);
  while (is_version_character(config.peek())) {
    char ch;
    config >> ch;
    version += ch;
  }
  return version;
}

bool is_package_name_character(char ch) {
  return isalnum(ch) || ch =='.' || ch == '\'' || ch == '_' || ch == '$';
}

std::string parse_package_name(std::istream& config, unsigned int* line) {
  skip_whitespace(config, line);
  std::string package_name;
  while (is_package_name_character(config.peek())) {
    char ch;
    config >> ch;
    package_name += ch;
  }
  return package_name;
}

bool lex_filter(std::istream& config, std::string* filter, unsigned int* line) {
  skip_whitespace(config, line);
  // Make sure we are not at the end of the file or the start of another command
  // when the argument is missing.
  if (config.peek() == EOF || config.peek() == '-') {
    return false;
  }
  *filter = "";
  while (config.peek() != ',' && !isspace(config.peek())
         && !(config.peek() == EOF)) {
    char ch;
    config >> ch;
    *filter += ch;
  }
  return true;
}

std::vector<std::string> lex_filter_list(
  std::istream& config,
  unsigned int* line
) {
  std::vector<std::string> filter_list;
  std::string filter;
  bool ok = lex_filter(config, &filter, line);
  if (!ok) {
    return filter_list;
  }
  filter_list.push_back(filter);
  while (ok && config.peek() == ',') {
    // Swallow up the comma.
    char ch ;
    config >> ch;
    ok = lex_filter(config, &filter, line);
    if (ok) {
      filter_list.push_back(filter);
    }
  }
  return filter_list;
}

std::vector<std::unique_ptr<Token>> lex(std::istream& config) {
  std::vector<std::unique_ptr<Token>> tokens;

  unsigned int line = 1;
  char ch;
  while (config >> std::noskipws >> ch) {

    // Skip comments.
    if (ch == '#') {
      std::string comment;
      getline(config,  comment);
      line++;
      continue;
    }

    // Skip white space.
    if (isspace(ch)) {
      if (ch == '\n') {
        line++;
      }
      continue;
    }

    if (ch == '{') {
      tokens.push_back(std::unique_ptr<Token>(new OpenCurlyBracket(line)));
      continue;
    }

    if (ch == '}') {
      tokens.push_back(std::unique_ptr<Token>(new CloseCurlyBracket(line)));
      continue;
    }

    if (ch == '(') {
      tokens.push_back(std::unique_ptr<Token>(new OpenBracket(line)));
      continue;
    }

    if (ch == ')') {
      tokens.push_back(std::unique_ptr<Token>(new CloseBracket(line)));
      continue;
    }

    if (ch == ';') {
      tokens.push_back(std::unique_ptr<Token>(new SemiColon(line)));
      continue;
    }

    if (ch == ':') {
      tokens.push_back(std::unique_ptr<Token>(new Colon(line)));
      continue;
    }

    if (ch == ',') {
      tokens.push_back(std::unique_ptr<Token>(new Comma(line)));
      continue;
    }

    if (ch == '!') {
      tokens.push_back(std::unique_ptr<Token>(new Not(line)));
      continue;
    }

    if (ch == '.') {
      tokens.push_back(std::unique_ptr<Token>(new Dot(line)));
      continue;
    }

    if (ch == '/') {
      tokens.push_back(std::unique_ptr<Token>(new Slash(line)));
      continue;
    }

    if (ch == '@') {
      tokens.push_back(std::unique_ptr<Token>(new AnnotationApplication(line)));
      continue;
    }

    if (ch == '[') {
      // Consume any whitespace
      while (isspace(config.peek())) {
        char skip;
        config >> std::noskipws >> skip;
        if (skip == '\n') {
          line++;
        }
      }
      // Check for closing brace.
      if (config.peek() == ']') {
        config >> ch;
        tokens.push_back(std::unique_ptr<Token>(new ArrayType(line)));
        continue;
      }
      // Any token other than a ']' next is a bad token.
    }

    if (ch == '<' ) {
      std::string word = "<";
      while (ch != '>' && ch != '\n') {
        config >> std::noskipws >> ch;
        word += ch;
      }
      if (ch == '\n') {
        line++;
      }
      // Check to see if we reached the end of the file before encountering
      // the closing angle brace.
      if (ch != '>') {
        tokens.push_back(std::unique_ptr<Token>(new UnknownToken(word, line)));
        continue;
      }
      if (word == "<init>") {
        tokens.push_back(std::unique_ptr<Token>(new Init(line)));
        continue;
      }
      if (word == "<fields>") {
        tokens.push_back(std::unique_ptr<Token>(new Fields(line)));
        continue;
      }
      if (word == "<methods>") {
        tokens.push_back(std::unique_ptr<Token>(new Methods(line)));
        continue;
      }
      tokens.push_back(std::unique_ptr<Token>(new UnknownToken(word, line)));
      continue;
    }

    // Check for commands.
    if (ch == '-') {
      std::string command;
      while (!is_deliminator(config.peek())) {
        config >> ch;
        command += ch;
      }

      // Input/Output Options
      if (command == "include") {
        tokens.push_back(std::unique_ptr<Token>(new Include(line)));
        std::string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "basedirectory") {
        tokens.push_back(std::unique_ptr<Token>(new BaseDirectory(line)));
        std::string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "injars") {
        tokens.push_back(std::unique_ptr<Token>(new InJars(line)));
        std::vector<std::string> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "outjars") {
        tokens.push_back(std::unique_ptr<Token>(new OutJars(line)));
        std::vector<std::string> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "libraryjars") {
        tokens.push_back(std::unique_ptr<Token>(new LibraryJars(line)));
        std::vector<std::string> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "printmapping") {
        tokens.push_back(std::unique_ptr<Token>(new PrintMapping(line)));
        std::string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "printconfiguration") {
        tokens.push_back(std::unique_ptr<Token>(new PrintConfiguration(line)));
        std::string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "printseeds") {
        tokens.push_back(std::unique_ptr<Token>(new PrintSeeds(line)));
        std::string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "target") {
        tokens.push_back(std::unique_ptr<Token>(new Target(line)));
        std::string version = read_target_version(config, &line);
        if (version != "") {
          tokens.push_back(
            std::unique_ptr<Token>(new TargetVersion(line, version)));
        }
        continue;
      }

      // Keep Options
      if (command == "keepdirectories") {
        tokens.push_back(std::unique_ptr<Token>(new KeepDirectories(line)));
        std::vector<std::string> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "keep") {
        tokens.push_back(std::unique_ptr<Token>(new Keep(line)));
        continue;
      }
      if (command == "keepclassmembers") {
        tokens.push_back(std::unique_ptr<Token>(new KeepClassMembers(line)));
        continue;
      }
      if (command == "keepclasseswithmembers") {
        tokens.push_back(
          std::unique_ptr<Token>(new KeepClassesWithMembers(line)));
        continue;
      }
      if (command == "keepnames") {
        tokens.push_back(std::unique_ptr<Token>(new KeepNames(line)));
        continue;
      }
      if (command == "keepclassmembernames") {
        tokens.push_back(
          std::unique_ptr<Token>(new KeepClassMemberNames(line)));
        continue;
      }
      if (command == "keepclasseswithmembernames") {
        tokens.push_back(
          std::unique_ptr<Token>(new KeepClassesWithMemberNames(line)));
        continue;
      }

      // Shrinking Options
      if (command == "dontshrink") {
        tokens.push_back(std::unique_ptr<Token>(new DontShrink(line)));
        continue;
      }
      if (command == "printusage") {
        tokens.push_back(std::unique_ptr<Token>(new PrintUsage(line)));
        std::string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(std::unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "whyareyoukeeping") {
        tokens.push_back(std::unique_ptr<Token>(new WhyAreYouKeeping(line)));
        continue;
      }

      // Optimization Options
      if (command == "optimizations") {
        tokens.push_back(std::unique_ptr<Token>(new Optimizations(line)));
        for (const auto& filter : lex_filter_list(config, &line)) {
          tokens.push_back(std::unique_ptr<Token>(new Filter(line, filter)));
        }
        continue;
      }
      if (command == "assumenosideeffects") {
        tokens.push_back(std::unique_ptr<Token>(new AssumeSideEffects(line)));
        continue;
      }
      if (command == "allowaccessmodification") {
        tokens.push_back(
          std::unique_ptr<Token>(new AllowAccessModification(line)));
        continue;
      }
      if (command == "dontoptimize") {
        tokens.push_back(std::unique_ptr<Token>(new DontOptimize(line)));
        continue;
      }
      if (command == "optimizationpasses") {
        tokens.push_back(std::unique_ptr<Token>(new OptimizationPasses(line)));
        continue;
      }
      if (command == "mergeinterfacesaggressively") {
        tokens.push_back(
          std::unique_ptr<Token>(new MergeInterfacesAggressively(line)));
        continue;
      }

      // Obfusication Options
      if (command == "repackageclasses") {
        tokens.push_back(std::unique_ptr<Token>(new RepackageClasses(line)));
        std::string package_name = parse_package_name(config, &line);
        if (package_name != "") {
          tokens.push_back(
            std::unique_ptr<Token>(new Identifier(line, package_name)));
        }
        continue;
      }
      if (command == "keepattributes") {
        tokens.push_back(std::unique_ptr<Token>(new KeepAttributes(line)));
        for (const auto& filter : lex_filter_list(config, &line)) {
          tokens.push_back(std::unique_ptr<Token>(new Filter(line, filter)));
        }
        continue;
      }
      if (command == "dontusemixedcaseclassnames") {
        tokens.push_back(
          std::unique_ptr<Token>(new DontUseMixedcaseClassNames(line)));
        continue;
      }

      // Preverification Options.
      if (command == "dontpreverify") {
        tokens.push_back(std::unique_ptr<Token>(new DontPreverify(line)));
        continue;
      }

      // General Options
      if (command == "dontwarn") {
        tokens.push_back(std::unique_ptr<Token>(new DontWarn(line)));
        for (const auto& filter : lex_filter_list(config, &line)) {
          tokens.push_back(std::unique_ptr<Token>(new Filter(line, filter)));
        }
        continue;
      }
      if (command == "verbose") {
        tokens.push_back(std::unique_ptr<Token>(new Verbose(line)));
        continue;
      }

      // Some other command.
      tokens.push_back(std::unique_ptr<Token>(new Command(line, command)));
      continue;

    }

    std::string word;
    word += ch;
    while (!is_deliminator(config.peek())) {
      config >> ch;
      word += ch;
    }

    if (word == "includedescriptorclasses") {
      tokens.push_back(
        std::unique_ptr<Token>(new IncludeDescriptorClasses(line)));
      continue;
    }

    if (word == "allowshrinking") {
      tokens.push_back(std::unique_ptr<Token>(new AllowShrinking(line)));
      continue;
    }

    if (word == "allowoptimization") {
      tokens.push_back(std::unique_ptr<Token>(new AllowOptimization(line)));
      continue;
    }

    if (word == "allowobfuscation") {
      tokens.push_back(std::unique_ptr<Token>(new AllowObfuscation(line)));
      continue;
    }

    if (word == "class") {
      tokens.push_back(std::unique_ptr<Token>(new Class(line)));
      continue;
    }

    if (word == "public") {
      tokens.push_back(std::unique_ptr<Token>(new Public(line)));
      continue;
    }

    if (word == "final") {
      tokens.push_back(std::unique_ptr<Token>(new Final(line)));
      continue;
    }

    if (word == "abstract") {
      tokens.push_back(std::unique_ptr<Token>(new Abstract(line)));
      continue;
    }

    if (word == "interface") {
      // If the previous symbol was a @ then this is really an annotation.
      if (!tokens.empty()
          && tokens.back()->type == token::annotation_application) {
        tokens.back()->type = token::annotation;
      } else {
        tokens.push_back(std::unique_ptr<Token>(new Interface(line)));
      }
      continue;
    }

    if (word == "enum") {
      tokens.push_back(std::unique_ptr<Token>(new Enum(line)));
      continue;
    }

    if (word == "private") {
      tokens.push_back(std::unique_ptr<Token>(new Private(line)));
      continue;
    }

    if (word == "protected") {
      tokens.push_back(std::unique_ptr<Token>(new Protected(line)));
      continue;
    }

    if (word == "static") {
      tokens.push_back(std::unique_ptr<Token>(new Static(line)));
      continue;
    }

    if (word == "volatile") {
      tokens.push_back(std::unique_ptr<Token>(new Volatile(line)));
      continue;
    }

    if (word == "transient") {
      tokens.push_back(std::unique_ptr<Token>(new Transient(line)));
      continue;
    }

    if (word == "synchronized") {
      tokens.push_back(std::unique_ptr<Token>(new Synchronized(line)));
      continue;
    }

    if (word == "native") {
      tokens.push_back(std::unique_ptr<Token>(new Native(line)));
      continue;
    }

    if (word == "strictfp") {
      tokens.push_back(std::unique_ptr<Token>(new Strictfp(line)));
      continue;
    }

    if (word == "synthetic") {
      tokens.push_back(std::unique_ptr<Token>(new Synthetic(line)));
      continue;
    }

    if (word == "bridge") {
      tokens.push_back(std::unique_ptr<Token>(new Bridge(line)));
      continue;
    }

    if (word == "varargs") {
      tokens.push_back(std::unique_ptr<Token>(new Varargs(line)));
      continue;
    }

    if (word == "extends") {
      tokens.push_back(std::unique_ptr<Token>(new Extends(line)));
      continue;
    }

    if (word == "implements") {
      tokens.push_back(std::unique_ptr<Token>(new Implements(line)));
      continue;
    }

    if (is_identifier(word)) {
      tokens.push_back(std::unique_ptr<Token>(new Identifier(line, word)));
      continue;
    }

    // This is an unrecognized token.
    tokens.push_back(std::unique_ptr<Token>(new UnknownToken(word, line)));
  }
  tokens.push_back(std::unique_ptr<Token>(new EndOfFile(line)));
  return tokens;
}

} // namespace proguard_parser
} // namespace redex
