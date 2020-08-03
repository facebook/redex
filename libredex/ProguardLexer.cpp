/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cctype>
#include <istream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Debug.h"
#include "ProguardLexer.h"

namespace keep_rules {
namespace proguard_parser {

namespace {

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

bool is_identifier(const std::string& ident) {
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
    config.get(ch);
    if (ch == '\n') {
      (*line)++;
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

std::vector<std::pair<std::string, unsigned int>> read_paths(
    std::istream& config, unsigned int* line) {
  std::vector<std::pair<std::string, unsigned int>> paths;
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

std::string read_target_version(std::istream& config, unsigned int* line) {
  std::string version;
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

std::string parse_package_name(std::istream& config, unsigned int* line) {
  skip_whitespace(config, line);
  std::string package_name;
  while (is_package_name_character(config.peek())) {
    char ch;
    config.get(ch);
    package_name += ch;
  }
  return package_name;
}

bool lex_filter(std::istream& config, std::string* filter, unsigned int* line) {
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

std::vector<std::string> lex_filter_list(std::istream& config,
                                         unsigned int* line) {
  std::vector<std::string> filter_list;
  std::string filter;
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

} // namespace

std::string Token::show() const {
  switch (type) {
  case TokenType::openCurlyBracket:
    return "{";
  case TokenType::closeCurlyBracket:
    return "}";
  case TokenType::openBracket:
    return "(";
  case TokenType::closeBracket:
    return ")";
  case TokenType::semiColon:
    return ";";
  case TokenType::colon:
    return ":";
  case TokenType::notToken:
    return "!";
  case TokenType::comma:
    return ",";
  case TokenType::slash:
    return "/";
  case TokenType::classToken:
    return "class";
  case TokenType::publicToken:
    return "public";
  case TokenType::final:
    return "final";
  case TokenType::abstract:
    return "abstract";
  case TokenType::interface:
    return "interface";
  case TokenType::enumToken:
    return "enum";
  case TokenType::extends:
    return "extends";
  case TokenType::implements:
    return "implements";
  case TokenType::privateToken:
    return "private";
  case TokenType::protectedToken:
    return "protected";
  case TokenType::staticToken:
    return "static";
  case TokenType::volatileToken:
    return "volatile";
  case TokenType::transient:
    return "transient";
  case TokenType::annotation:
    return "@interface";
  case TokenType::annotation_application:
    return "@";
  case TokenType::synchronized:
    return "synchronized";
  case TokenType::native:
    return "native";
  case TokenType::strictfp:
    return "strictfp";
  case TokenType::synthetic:
    return "synthetic";
  case TokenType::bridge:
    return "bridge";
  case TokenType::varargs:
    return "varargs";
  case TokenType::command:
    return "-" + *data;
  case TokenType::identifier:
    return "identifier: " + *data;
  case TokenType::arrayType:
    return "[]";
  case TokenType::filepath:
    return "filepath " + *data;
  case TokenType::target_version_token:
    return *data;
  case TokenType::filter_pattern:
    return "filter: " + *data;
  case TokenType::eof_token:
    return "<EOF>";

  // Input/Output Options
  case TokenType::include:
    return "-include";
  case TokenType::basedirectory:
    return "-basedirectory";
  case TokenType::injars:
    return "-injars ";
  case TokenType::outjars:
    return "-outjars ";
  case TokenType::libraryjars:
    return "-libraryjars ";
  case TokenType::keepdirectories:
    return "-keepdirectories";
  case TokenType::target:
    return "-target ";
  case TokenType::dontskipnonpubliclibraryclasses:
    return "-dontskipnonpubliclibraryclasses";

  // Keep Options
  case TokenType::keep:
    return "-keep";
  case TokenType::keepclassmembers:
    return "-keepclassmembers";
  case TokenType::keepclasseswithmembers:
    return "-keepclasseswithmembers";
  case TokenType::keepnames:
    return "-keepnames";
  case TokenType::keepclassmembernames:
    return "-keepclassmembernames";
  case TokenType::keepclasseswithmembernames:
    return "-keepclasseswithmembernames";
  case TokenType::printseeds:
    return "-printseeds ";

  // Keep Option Modifiers
  case TokenType::includedescriptorclasses_token:
    return "includedescriptorclasses";
  case TokenType::allowshrinking_token:
    return "allowshrinking";
  case TokenType::allowoptimization_token:
    return "allowoptimization";
  case TokenType::allowobfuscation_token:
    return "allowobfuscation";

  // Shrinking Options
  case TokenType::dontshrink:
    return "-dontshrink";
  case TokenType::printusage:
    return "-printusage";
  case TokenType::whyareyoukeeping:
    return "-whyareyoukeeping";

  // Optimization Options
  case TokenType::dontoptimize:
    return "-dontoptimize";
  case TokenType::optimizations:
    return "-optimizations";
  case TokenType::optimizationpasses:
    return "-optimizationpasses";
  case TokenType::assumenosideeffects:
    return "-assumenosideeffects";
  case TokenType::mergeinterfacesaggressively:
    return "-mergeinterfacesaggressively";
  case TokenType::allowaccessmodification_token:
    return "-allowaccessmodification";

  // Obfuscation Options
  case TokenType::dontobfuscate:
    return "-dontobfuscate ";
  case TokenType::printmapping:
    return "-printmapping ";
  case TokenType::repackageclasses:
    return "-repackageclasses";
  case TokenType::keepattributes:
    return "-keepattributes";
  case TokenType::dontusemixedcaseclassnames_token:
    return "-dontusemixedcaseclassnames";
  case TokenType::keeppackagenames:
    return "-keeppackagenames";

  // Preverification Options
  case TokenType::dontpreverify_token:
    return "-dontpreverify";

  // General Options
  case TokenType::printconfiguration:
    return "-printconfiguration ";
  case TokenType::dontwarn:
    return "-dontwarn";
  case TokenType::verbose_token:
    return "-verbose";

  case TokenType::unknownToken:
    return "unknown token at line " + std::to_string(line) + " : " + *data;
  }
  not_reached();
}

bool Token::is_command() const {
  switch (type) {
  case TokenType::openCurlyBracket:
  case TokenType::closeCurlyBracket:
  case TokenType::openBracket:
  case TokenType::closeBracket:
  case TokenType::semiColon:
  case TokenType::colon:
  case TokenType::notToken:
  case TokenType::comma:
  case TokenType::slash:
  case TokenType::classToken:
  case TokenType::publicToken:
  case TokenType::final:
  case TokenType::abstract:
  case TokenType::interface:
  case TokenType::enumToken:
  case TokenType::extends:
  case TokenType::implements:
  case TokenType::privateToken:
  case TokenType::protectedToken:
  case TokenType::staticToken:
  case TokenType::volatileToken:
  case TokenType::transient:
  case TokenType::annotation:
  case TokenType::annotation_application:
  case TokenType::synchronized:
  case TokenType::native:
  case TokenType::strictfp:
  case TokenType::synthetic:
  case TokenType::bridge:
  case TokenType::varargs:
  case TokenType::identifier:
  case TokenType::arrayType:
  case TokenType::filepath:
  case TokenType::target_version_token:
  case TokenType::filter_pattern:
  case TokenType::eof_token:
    return false;

  case TokenType::command:
    return true;

  // Input/Output Options
  case TokenType::include:
  case TokenType::basedirectory:
  case TokenType::injars:
  case TokenType::outjars:
  case TokenType::libraryjars:
  case TokenType::keepdirectories:
  case TokenType::target:
  case TokenType::dontskipnonpubliclibraryclasses:
    return true;

  // Keep Options
  case TokenType::keep:
  case TokenType::keepclassmembers:
  case TokenType::keepclasseswithmembers:
  case TokenType::keepnames:
  case TokenType::keepclassmembernames:
  case TokenType::keepclasseswithmembernames:
  case TokenType::printseeds:
    return true;

  // Keep Option Modifiers
  case TokenType::includedescriptorclasses_token:
  case TokenType::allowshrinking_token:
  case TokenType::allowoptimization_token:
  case TokenType::allowobfuscation_token:
    return false;

  // Shrinking Options
  case TokenType::dontshrink:
  case TokenType::printusage:
  case TokenType::whyareyoukeeping:
    return true;

  // Optimization Options
  case TokenType::dontoptimize:
  case TokenType::optimizations:
  case TokenType::optimizationpasses:
  case TokenType::assumenosideeffects:
  case TokenType::mergeinterfacesaggressively:
  case TokenType::allowaccessmodification_token:
    return true;

  // Obfuscation Options
  case TokenType::dontobfuscate:
  case TokenType::printmapping:
  case TokenType::repackageclasses:
  case TokenType::keepattributes:
  case TokenType::dontusemixedcaseclassnames_token:
  case TokenType::keeppackagenames:
    return true;

  // Preverification Options
  case TokenType::dontpreverify_token:
    return true;

  // General Options
  case TokenType::printconfiguration:
  case TokenType::dontwarn:
  case TokenType::verbose_token:
    return true;

  case TokenType::unknownToken:
    return false;
  }
  not_reached();
}

std::vector<Token> lex(std::istream& config) {
  std::vector<Token> tokens;

  unsigned int line = 1;
  char ch;

  std::unordered_map<char, TokenType> simple_tokens{
      {'{', TokenType::openCurlyBracket},
      {'}', TokenType::closeCurlyBracket},
      {'(', TokenType::openBracket},
      {')', TokenType::closeBracket},
      {';', TokenType::semiColon},
      {':', TokenType::colon},
      {',', TokenType::comma},
      {'!', TokenType::notToken},
      {'/', TokenType::slash},
      {'@', TokenType::annotation_application},
  };

  std::unordered_map<std::string, TokenType> word_tokens{
      {"includedescriptorclasses", TokenType::includedescriptorclasses_token},
      {"allowshrinking", TokenType::allowshrinking_token},
      {"allowoptimization", TokenType::allowoptimization_token},
      {"allowobfuscation", TokenType::allowobfuscation_token},
      {"class", TokenType::classToken},
      {"public", TokenType::publicToken},
      {"final", TokenType::final},
      {"abstract", TokenType::abstract},
      {"enum", TokenType::enumToken},
      {"private", TokenType::privateToken},
      {"protected", TokenType::protectedToken},
      {"static", TokenType::staticToken},
      {"volatile", TokenType::volatileToken},
      {"transient", TokenType::transient},
      {"synchronized", TokenType::synchronized},
      {"native", TokenType::native},
      {"strictfp", TokenType::strictfp},
      {"synthetic", TokenType::synthetic},
      {"bridge", TokenType::bridge},
      {"varargs", TokenType::varargs},
      {"extends", TokenType::extends},
      {"implements", TokenType::implements},
  };

  std::unordered_map<std::string, TokenType> simple_commands{
      // Keep Options
      {"keep", TokenType::keep},
      {"keepclassmembers", TokenType::keepclassmembers},
      {"keepclasseswithmembers", TokenType::keepclasseswithmembers},
      {"keepnames", TokenType::keepnames},
      {"keepclassmembernames", TokenType::keepclassmembernames},
      {"keepclasseswithmembernames", TokenType::keepclasseswithmembernames},

      // Shrinking Options
      {"dontshrink", TokenType::dontshrink},

      {"whyareyoukeeping", TokenType::whyareyoukeeping},

      // Optimization Options
      {"assumenosideeffects", TokenType::assumenosideeffects},
      {"allowaccessmodification", TokenType::allowaccessmodification_token},
      {"dontoptimize", TokenType::dontoptimize},
      {"optimizationpasses", TokenType::optimizationpasses},
      {"mergeinterfacesaggressively", TokenType::mergeinterfacesaggressively},

      // Obfuscation Options
      {"dontobfuscate", TokenType::dontobfuscate},
      {"dontusemixedcaseclassnames",
       TokenType::dontusemixedcaseclassnames_token},
      {"dontskipnonpubliclibraryclasses",
       TokenType::dontskipnonpubliclibraryclasses},
      {"keeppackagenames", TokenType::keeppackagenames},

      // Preverification Options.
      {"dontpreverify", TokenType::dontpreverify_token},

      // General Options
      {"verbose", TokenType::verbose_token},
  };

  std::unordered_map<std::string, TokenType> single_filepath_commands{
      // Input/Output Options
      {"include", TokenType::include},
      {"basedirectory", TokenType::basedirectory},
      {"printmapping", TokenType::printmapping},
      {"printconfiguration", TokenType::printconfiguration},
      {"printseeds", TokenType::printseeds},
      // Shrinking Options
      {"printusage", TokenType::printusage},
  };
  std::unordered_map<std::string, TokenType> multi_filepaths_commands{
      // Input/Output Options
      {"injars", TokenType::injars},
      {"outjars", TokenType::outjars},
      {"libraryjars", TokenType::libraryjars},
      // Keep Options
      {"keepdirectories", TokenType::keepdirectories},
  };

  std::unordered_map<std::string, TokenType> filter_list_commands{
      // Optimization Options
      {"optimizations", TokenType::optimizations},
      // Obfuscation Options
      {"keepattributes", TokenType::keepattributes},
      // General Options
      {"dontwarn", TokenType::dontwarn},
  };

  auto add_token = [&](TokenType type) { tokens.emplace_back(type, line); };
  auto add_token_data = [&](TokenType type, std::string&& data) {
    tokens.emplace_back(type, line, std::move(data));
  };
  auto add_token_line_data =
      [&](TokenType type, size_t t_line, std::string&& data) {
        tokens.emplace_back(type, t_line, std::move(data));
      };

  while (config.get(ch)) {
    // Skip comments.
    if (ch == '#') {
      std::string comment;
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

    {
      auto it = simple_tokens.find(ch);
      if (it != simple_tokens.end()) {
        add_token(it->second);
        continue;
      }
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
        add_token(TokenType::arrayType);
        continue;
      }
      // Any token other than a ']' next is a bad token.
    }

    // Check for commands.
    if (ch == '-') {
      std::string command;
      while (!is_deliminator(config.peek())) {
        config.get(ch);
        command += ch;
      }

      {
        auto it = simple_commands.find(command);
        if (it != simple_commands.end()) {
          add_token(it->second);
          continue;
        }
      }

      {
        auto it = single_filepath_commands.find(command);
        if (it != single_filepath_commands.end()) {
          add_token(it->second);
          std::string path = read_path(config, &line);
          if (!path.empty()) {
            add_token_data(TokenType::filepath, std::move(path));
          }
          continue;
        }
      }

      {
        auto it = multi_filepaths_commands.find(command);
        if (it != multi_filepaths_commands.end()) {
          add_token(it->second);
          auto paths = read_paths(config, &line);
          for (auto& path : paths) {
            add_token_line_data(
                TokenType::filepath, path.second, std::move(path.first));
          }
          continue;
        }
      }

      {
        auto it = filter_list_commands.find(command);
        if (it != filter_list_commands.end()) {
          add_token(it->second);
          for (auto& filter : lex_filter_list(config, &line)) {
            add_token_data(TokenType::filter_pattern, std::move(filter));
          }
          continue;
        }
      }

      // Input/Output Options
      if (command == "target") {
        add_token(TokenType::target);
        std::string version = read_target_version(config, &line);
        if (!version.empty()) {
          add_token_data(TokenType::target_version_token, std::move(version));
        }
        continue;
      }

      // Obfuscation Options
      if (command == "repackageclasses") {
        add_token(TokenType::repackageclasses);
        std::string package_name = parse_package_name(config, &line);
        if (!package_name.empty()) {
          add_token_data(TokenType::identifier, std::move(package_name));
        }
        continue;
      }

      // Some other command.
      add_token_data(TokenType::command, std::move(command));
      continue;
    }

    std::string word;
    word += ch;
    while (!is_deliminator(config.peek())) {
      config >> ch;
      word += ch;
    }

    {
      auto it = word_tokens.find(word);
      if (it != word_tokens.end()) {
        add_token(it->second);
        continue;
      }
    }

    if (word == "interface") {
      // If the previous symbol was a @ then this is really an annotation.
      if (!tokens.empty() &&
          tokens.back().type == TokenType::annotation_application) {
        tokens.pop_back();
        add_token(TokenType::annotation);
      } else {
        add_token(TokenType::interface);
      }
      continue;
    }

    if (is_identifier(word)) {
      add_token_data(TokenType::identifier, std::move(word));
      continue;
    }

    // This is an unrecognized token.
    add_token_data(TokenType::unknownToken, std::move(word));
  }
  add_token(TokenType::eof_token);
  return tokens;
}

} // namespace proguard_parser
} // namespace keep_rules
