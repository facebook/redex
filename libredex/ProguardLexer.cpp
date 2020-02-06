/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "Debug.h"
#include "ProguardLexer.h"

namespace keep_rules {
namespace proguard_parser {

bool is_deliminator(char ch) {
  return isspace(ch) || ch == '{' || ch == '}' || ch == '(' || ch == ')' ||
         ch == ',' || ch == ';' || ch == ':' || ch == EOF;
}

// An identifier can refer to a class name, a field name or a package name.
bool is_identifier_character(char ch) {
  return isalnum(ch) || ch == '_' || ch == '$' || ch == '*' || ch == '.' ||
         ch == '\'' || ch == '[' || ch == ']' || ch == '<' || ch == '>' ||
         ch == '!' || ch == '?' || ch == '%';
}

bool is_identifier(const string& ident) {
  for (const char& ch : ident) {
    if (!is_identifier_character(ch)) {
      return false;
    }
  }
  return true;
}

void skip_whitespace(istream& config, unsigned int* line) {
  char ch;
  while (isspace(config.peek())) {
    config.get(ch);
    if (ch == '\n') {
      (*line)++;
    }
  }
}

string read_path(istream& config, unsigned int* line) {
  string path;
  skip_whitespace(config, line);
  // Handle the case for optional filepath arguments by
  // returning an empty filepath.
  if (config.peek() == '-' || config.peek() == EOF) {
    return "";
  }
  while (config.peek() != ':' && !isspace(config.peek()) &&
         config.peek() != EOF) {
    char ch;
    config.get(ch);
    if (ch == '"') {
      continue;
    }
    path += ch;
  }
  return path;
}

vector<pair<string, unsigned int>> read_paths(istream& config,
                                              unsigned int* line) {
  vector<pair<string, unsigned int>> paths;
  paths.push_back({read_path(config, line), *line});
  skip_whitespace(config, line);
  while (config.peek() == ':') {
    char ch;
    config.get(ch);
    paths.push_back({read_path(config, line), *line});
    skip_whitespace(config, line);
  }
  return paths;
}

bool is_version_character(char ch) { return ch == '.' || isdigit(ch); }

string read_target_version(istream& config, unsigned int* line) {
  string version;
  skip_whitespace(config, line);
  while (is_version_character(config.peek())) {
    char ch;
    config.get(ch);
    version += ch;
  }
  return version;
}

bool is_package_name_character(char ch) {
  return isalnum(ch) || ch == '.' || ch == '\'' || ch == '_' || ch == '$';
}

string parse_package_name(istream& config, unsigned int* line) {
  skip_whitespace(config, line);
  string package_name;
  while (is_package_name_character(config.peek())) {
    char ch;
    config.get(ch);
    package_name += ch;
  }
  return package_name;
}

bool lex_filter(istream& config, string* filter, unsigned int* line) {
  skip_whitespace(config, line);
  // Make sure we are not at the end of the file or the start of another
  // command when the argument is missing.
  if (config.peek() == EOF || config.peek() == '-') {
    return false;
  }
  *filter = "";
  while (config.peek() != ',' && !isspace(config.peek()) &&
         !(config.peek() == EOF)) {
    char ch;
    config.get(ch);
    *filter += ch;
  }
  return true;
}

vector<string> lex_filter_list(istream& config, unsigned int* line) {
  vector<string> filter_list;
  string filter;
  bool ok = lex_filter(config, &filter, line);
  if (!ok) {
    return filter_list;
  }
  filter_list.push_back(filter);
  skip_whitespace(config, line);
  while (ok && config.peek() == ',') {
    // Swallow up the comma.
    char ch;
    config.get(ch);
    ok = lex_filter(config, &filter, line);
    if (ok) {
      filter_list.push_back(filter);
      skip_whitespace(config, line);
    }
  }
  return filter_list;
}

vector<unique_ptr<Token>> lex(istream& config) {
  std::vector<unique_ptr<Token>> tokens;

  unsigned int line = 1;
  char ch;
  while (config.get(ch)) {

    // Skip comments.
    if (ch == '#') {
      string comment;
      getline(config, comment);
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
      tokens.push_back(unique_ptr<Token>(new OpenCurlyBracket(line)));
      continue;
    }

    if (ch == '}') {
      tokens.push_back(unique_ptr<Token>(new CloseCurlyBracket(line)));
      continue;
    }

    if (ch == '(') {
      tokens.push_back(unique_ptr<Token>(new OpenBracket(line)));
      continue;
    }

    if (ch == ')') {
      tokens.push_back(unique_ptr<Token>(new CloseBracket(line)));
      continue;
    }

    if (ch == ';') {
      tokens.push_back(unique_ptr<Token>(new SemiColon(line)));
      continue;
    }

    if (ch == ':') {
      tokens.push_back(unique_ptr<Token>(new Colon(line)));
      continue;
    }

    if (ch == ',') {
      tokens.push_back(unique_ptr<Token>(new Comma(line)));
      continue;
    }

    if (ch == '!') {
      tokens.push_back(unique_ptr<Token>(new Not(line)));
      continue;
    }

    if (ch == '/') {
      tokens.push_back(unique_ptr<Token>(new Slash(line)));
      continue;
    }

    if (ch == '@') {
      tokens.push_back(unique_ptr<Token>(new AnnotationApplication(line)));
      continue;
    }

    if (ch == '[') {
      // Consume any whitespace
      while (isspace(config.peek())) {
        char skip;
        config.get(skip);
        if (skip == '\n') {
          line++;
        }
      }
      // Check for closing brace.
      if (config.peek() == ']') {
        config.get(ch);
        tokens.push_back(unique_ptr<Token>(new ArrayType(line)));
        continue;
      }
      // Any token other than a ']' next is a bad token.
    }

    // Check for commands.
    if (ch == '-') {
      string command;
      while (!is_deliminator(config.peek())) {
        config.get(ch);
        command += ch;
      }

      // Input/Output Options
      if (command == "include") {
        tokens.push_back(unique_ptr<Token>(new Include(line)));
        string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "basedirectory") {
        tokens.push_back(unique_ptr<Token>(new BaseDirectory(line)));
        string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "injars") {
        tokens.push_back(unique_ptr<Token>(new InJars(line)));
        vector<pair<string, unsigned int>> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(
              unique_ptr<Token>(new Filepath(path.second, path.first)));
        }
        continue;
      }
      if (command == "outjars") {
        tokens.push_back(unique_ptr<Token>(new OutJars(line)));
        vector<pair<string, unsigned int>> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(
              unique_ptr<Token>(new Filepath(path.second, path.first)));
        }
        continue;
      }
      if (command == "libraryjars") {
        tokens.push_back(unique_ptr<Token>(new LibraryJars(line)));
        vector<pair<string, unsigned int>> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(
              unique_ptr<Token>(new Filepath(path.second, path.first)));
        }
        continue;
      }
      if (command == "printmapping") {
        tokens.push_back(unique_ptr<Token>(new PrintMapping(line)));
        string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "printconfiguration") {
        tokens.push_back(unique_ptr<Token>(new PrintConfiguration(line)));
        string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "printseeds") {
        tokens.push_back(unique_ptr<Token>(new PrintSeeds(line)));
        string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "target") {
        tokens.push_back(unique_ptr<Token>(new Target(line)));
        string version = read_target_version(config, &line);
        if (version != "") {
          tokens.push_back(unique_ptr<Token>(new TargetVersion(line, version)));
        }
        continue;
      }

      // Keep Options
      if (command == "keepdirectories") {
        tokens.push_back(unique_ptr<Token>(new KeepDirectories(line)));
        vector<pair<string, unsigned int>> paths = read_paths(config, &line);
        for (const auto& path : paths) {
          tokens.push_back(
              unique_ptr<Token>(new Filepath(path.second, path.first)));
        }
        continue;
      }
      if (command == "keep") {
        tokens.push_back(unique_ptr<Token>(new Keep(line)));
        continue;
      }
      if (command == "keepclassmembers") {
        tokens.push_back(unique_ptr<Token>(new KeepClassMembers(line)));
        continue;
      }
      if (command == "keepclasseswithmembers") {
        tokens.push_back(unique_ptr<Token>(new KeepClassesWithMembers(line)));
        continue;
      }
      if (command == "keepnames") {
        tokens.push_back(unique_ptr<Token>(new KeepNames(line)));
        continue;
      }
      if (command == "keepclassmembernames") {
        tokens.push_back(unique_ptr<Token>(new KeepClassMemberNames(line)));
        continue;
      }
      if (command == "keepclasseswithmembernames") {
        tokens.push_back(
            unique_ptr<Token>(new KeepClassesWithMemberNames(line)));
        continue;
      }

      // Shrinking Options
      if (command == "dontshrink") {
        tokens.push_back(unique_ptr<Token>(new DontShrink(line)));
        continue;
      }
      if (command == "printusage") {
        tokens.push_back(unique_ptr<Token>(new PrintUsage(line)));
        string path = read_path(config, &line);
        if (path != "") {
          tokens.push_back(unique_ptr<Token>(new Filepath(line, path)));
        }
        continue;
      }
      if (command == "whyareyoukeeping") {
        tokens.push_back(unique_ptr<Token>(new WhyAreYouKeeping(line)));
        continue;
      }

      // Optimization Options
      if (command == "optimizations") {
        tokens.push_back(unique_ptr<Token>(new Optimizations(line)));
        for (const auto& filter : lex_filter_list(config, &line)) {
          tokens.push_back(unique_ptr<Token>(new Filter(line, filter)));
        }
        continue;
      }
      if (command == "assumenosideeffects") {
        tokens.push_back(unique_ptr<Token>(new AssumeSideEffects(line)));
        continue;
      }
      if (command == "allowaccessmodification") {
        tokens.push_back(unique_ptr<Token>(new AllowAccessModification(line)));
        continue;
      }
      if (command == "dontoptimize") {
        tokens.push_back(unique_ptr<Token>(new DontOptimize(line)));
        continue;
      }
      if (command == "optimizationpasses") {
        tokens.push_back(unique_ptr<Token>(new OptimizationPasses(line)));
        continue;
      }
      if (command == "mergeinterfacesaggressively") {
        tokens.push_back(
            unique_ptr<Token>(new MergeInterfacesAggressively(line)));
        continue;
      }

      // Obfusication Options
      if (command == "dontobfuscate") {
        tokens.push_back(unique_ptr<Token>(new DontObfuscate(line)));
        continue;
      }
      if (command == "repackageclasses") {
        tokens.push_back(unique_ptr<Token>(new RepackageClasses(line)));
        string package_name = parse_package_name(config, &line);
        if (package_name != "") {
          tokens.push_back(
              unique_ptr<Token>(new Identifier(line, package_name)));
        }
        continue;
      }
      if (command == "keepattributes") {
        tokens.push_back(unique_ptr<Token>(new KeepAttributes(line)));
        for (const auto& filter : lex_filter_list(config, &line)) {
          tokens.push_back(unique_ptr<Token>(new Filter(line, filter)));
        }
        continue;
      }
      if (command == "dontusemixedcaseclassnames") {
        tokens.push_back(
            unique_ptr<Token>(new DontUseMixedcaseClassNames(line)));
        continue;
      }
      if (command == "dontskipnonpubliclibraryclasses") {
        tokens.push_back(
            unique_ptr<Token>(new DontSkipNonPublicLibraryClasses(line)));
        continue;
      }
      if (command == "keeppackagenames") {
        tokens.push_back(unique_ptr<Token>(new KeepPackageNames(line)));
        continue;
      }

      // Preverification Options.
      if (command == "dontpreverify") {
        tokens.push_back(unique_ptr<Token>(new DontPreverify(line)));
        continue;
      }

      // General Options
      if (command == "dontwarn") {
        tokens.push_back(unique_ptr<Token>(new DontWarn(line)));
        for (const auto& filter : lex_filter_list(config, &line)) {
          tokens.push_back(unique_ptr<Token>(new Filter(line, filter)));
        }
        continue;
      }
      if (command == "verbose") {
        tokens.push_back(unique_ptr<Token>(new Verbose(line)));
        continue;
      }

      // Some other command.
      tokens.push_back(unique_ptr<Token>(new Command(line, command)));
      continue;
    }

    string word;
    word += ch;
    while (!is_deliminator(config.peek())) {
      config >> ch;
      word += ch;
    }

    if (word == "includedescriptorclasses") {
      tokens.push_back(unique_ptr<Token>(new IncludeDescriptorClasses(line)));
      continue;
    }

    if (word == "allowshrinking") {
      tokens.push_back(unique_ptr<Token>(new AllowShrinking(line)));
      continue;
    }

    if (word == "allowoptimization") {
      tokens.push_back(unique_ptr<Token>(new AllowOptimization(line)));
      continue;
    }

    if (word == "allowobfuscation") {
      tokens.push_back(unique_ptr<Token>(new AllowObfuscation(line)));
      continue;
    }

    if (word == "class") {
      tokens.push_back(unique_ptr<Token>(new Class(line)));
      continue;
    }

    if (word == "public") {
      tokens.push_back(unique_ptr<Token>(new Public(line)));
      continue;
    }

    if (word == "final") {
      tokens.push_back(unique_ptr<Token>(new Final(line)));
      continue;
    }

    if (word == "abstract") {
      tokens.push_back(unique_ptr<Token>(new Abstract(line)));
      continue;
    }

    if (word == "interface") {
      // If the previous symbol was a @ then this is really an annotation.
      if (!tokens.empty() &&
          tokens.back()->type == token::annotation_application) {
        tokens.pop_back();
        tokens.push_back(unique_ptr<Token>(new Annotation(line)));
      } else {
        tokens.push_back(unique_ptr<Token>(new Interface(line)));
      }
      continue;
    }

    if (word == "enum") {
      tokens.push_back(unique_ptr<Token>(new Enum(line)));
      continue;
    }

    if (word == "private") {
      tokens.push_back(unique_ptr<Token>(new Private(line)));
      continue;
    }

    if (word == "protected") {
      tokens.push_back(unique_ptr<Token>(new Protected(line)));
      continue;
    }

    if (word == "static") {
      tokens.push_back(unique_ptr<Token>(new Static(line)));
      continue;
    }

    if (word == "volatile") {
      tokens.push_back(unique_ptr<Token>(new Volatile(line)));
      continue;
    }

    if (word == "transient") {
      tokens.push_back(unique_ptr<Token>(new Transient(line)));
      continue;
    }

    if (word == "synchronized") {
      tokens.push_back(unique_ptr<Token>(new Synchronized(line)));
      continue;
    }

    if (word == "native") {
      tokens.push_back(unique_ptr<Token>(new Native(line)));
      continue;
    }

    if (word == "strictfp") {
      tokens.push_back(unique_ptr<Token>(new Strictfp(line)));
      continue;
    }

    if (word == "synthetic") {
      tokens.push_back(unique_ptr<Token>(new Synthetic(line)));
      continue;
    }

    if (word == "bridge") {
      tokens.push_back(unique_ptr<Token>(new Bridge(line)));
      continue;
    }

    if (word == "varargs") {
      tokens.push_back(unique_ptr<Token>(new Varargs(line)));
      continue;
    }

    if (word == "extends") {
      tokens.push_back(unique_ptr<Token>(new Extends(line)));
      continue;
    }

    if (word == "implements") {
      tokens.push_back(unique_ptr<Token>(new Implements(line)));
      continue;
    }

    if (is_identifier(word)) {
      tokens.push_back(unique_ptr<Token>(new Identifier(line, word)));
      continue;
    }

    // This is an unrecognized token.
    tokens.push_back(unique_ptr<Token>(new UnknownToken(word, line)));
  }
  tokens.push_back(unique_ptr<Token>(new EndOfFile(line)));
  return tokens;
}

} // namespace proguard_parser
} // namespace keep_rules
