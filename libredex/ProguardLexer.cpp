/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index_container.hpp>
#include <cctype>
#include <istream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Debug.h"
#include "Macros.h"
#include "ProguardLexer.h"

namespace keep_rules {
namespace proguard_parser {

namespace {

constexpr char kPathDelim =
#if IS_WINDOWS
    ';';
#else
    ':';
#endif

bool is_deliminator(char ch) {
  return isspace(ch) || ch == '{' || ch == '}' || ch == '(' || ch == ')' ||
         ch == ',' || ch == ';' || ch == ':' || ch == EOF || ch == '#';
}

bool is_not_idenfitier_character(char ch) {
  return ch == '=' || ch == '+' || ch == '|' || ch == '@' || ch == '#' ||
         ch == '^' || ch == '&' || ch == '"' || ch == '\'' || ch == '`' ||
         ch == '~' || ch == '-';
}

// An identifier can refer to a class name, a field name or a package name.
// https://docs.oracle.com/javase/specs/jls/se16/html/jls-3.html#jls-JavaLetter
bool is_identifier(const boost::string_view& ident) {
  for (const char& ch : ident) {
    // java identifiers can be multi-lingual so membership testing is complex.
    // much simpler to test for what is definitely not an identifier and then
    // assume everything else is a legal identifier char, accepting that we
    // will have false positives.
    if (is_deliminator(ch) || is_not_idenfitier_character(ch)) {
      return false;
    }
  }
  return true;
}

void skip_whitespace(boost::string_view& data, unsigned int* line) {
  size_t index = 0;
  for (; index != data.size(); ++index) {
    char ch = data[index];
    if (ch == '\n') {
      (*line)++;
    }
    if (!isspace(ch)) {
      break;
    }
  }
  if (index == data.size()) {
    data = boost::string_view();
  } else {
    data = data.substr(index);
  }
}

boost::string_view read_path(boost::string_view& data, unsigned int* line) {
  skip_whitespace(data, line);
  // Handle the case for optional filepath arguments by
  // returning an empty filepath.
  if (data.empty() || data[0] == '-') {
    return boost::string_view();
  }

  bool has_quotes = data[0] == '"';
  size_t start = has_quotes ? 1 : 0;

  size_t end = start;
  for (; end != data.size(); ++end) {
    char c = data[end];
    if (c == kPathDelim || (!has_quotes && isspace(c))) {
      break;
    }
    if (c == '"' && has_quotes) {
      ++end;
      break;
    }
  }

  if (start == end) {
    data = data.substr(start);
    return boost::string_view(); // Should maybe be an error.
  }

  size_t adjusted_end = end;
  if (has_quotes && data[adjusted_end - 1] == '"') {
    --adjusted_end;
  }
  auto ret = data.substr(start, adjusted_end - start);
  data = data.substr(end);
  return ret;
}

std::vector<std::pair<boost::string_view, unsigned int>> read_paths(
    boost::string_view& data, unsigned int* line) {
  std::vector<std::pair<boost::string_view, unsigned int>> paths;
  paths.push_back({read_path(data, line), *line});
  skip_whitespace(data, line);
  while (!data.empty() && data[0] == kPathDelim) {
    data = data.substr(1);
    paths.push_back({read_path(data, line), *line});
    skip_whitespace(data, line);
  }
  return paths;
}

template <bool kSkipWs, typename FilterFn>
boost::string_view parse_part_fn(boost::string_view& data,
                                 unsigned int* line,
                                 FilterFn fn) {
  if (kSkipWs) {
    skip_whitespace(data, line);
  }
  auto first_delim = std::find_if(data.begin(), data.end(), fn);
  auto part = first_delim != data.end()
                  ? data.substr(0, first_delim - data.begin())
                  : data;
  data = first_delim != data.end() ? data.substr(first_delim - data.begin())
                                   : boost::string_view();
  return part;
}

boost::string_view read_target_version(boost::string_view& data,
                                       unsigned int* line) {
  auto is_version_character = [](char ch) { return ch == '.' || isdigit(ch); };
  return parse_part_fn</*kSkipWs=*/true>(
      data, line, [&](char c) { return !is_version_character(c); });
}

boost::string_view parse_package_name(boost::string_view& data,
                                      unsigned int* line) {
  auto pkg_name_char = [](char ch) {
    return isalnum(ch) || ch == '.' || ch == '\'' || ch == '_' || ch == '$';
  };
  return parse_part_fn</*kSkipWs=*/true>(
      data, line, [&](char c) { return !pkg_name_char(c); });
}

bool lex_filter(boost::string_view& data,
                boost::string_view* filter,
                unsigned int* line) {
  skip_whitespace(data, line);
  // Make sure we are not at the end of the file or the start of another
  // command when the argument is missing.
  if (data.empty() || data[0] == '-') {
    return false;
  }
  *filter = parse_part_fn</*kSkipWs=*/false>(
      data, line, [](char c) { return c == ',' || isspace(c); });
  return true;
}

std::vector<boost::string_view> lex_filter_list(boost::string_view& data,
                                                unsigned int* line) {
  std::vector<boost::string_view> filter_list;
  boost::string_view filter;
  bool ok = lex_filter(data, &filter, line);
  if (!ok) {
    return filter_list;
  }
  filter_list.push_back(filter);
  skip_whitespace(data, line);
  while (ok && !data.empty() && data[0] == ',') {
    // Swallow up the comma.
    data = data.substr(1);
    ok = lex_filter(data, &filter, line);
    if (ok) {
      filter_list.push_back(filter);
      skip_whitespace(data, line);
    }
  }
  return filter_list;
}

// std::unordered_map does not work with string views. Use Boost magic.
template <typename T, typename Q>
struct MyPair {
  T first;
  mutable Q second;
};

struct StringViewEquals {
  bool operator()(const std::string& s1, const std::string& s2) const {
    return s1 == s2;
  }
  bool operator()(const std::string& s1, const boost::string_view& v2) const {
    return v2 == s1;
  }
  bool operator()(const boost::string_view& v1, const std::string& s2) const {
    return v1 == s2;
  }
  bool operator()(const boost::string_view& v1,
                  const boost::string_view& v2) const {
    return v1 == v2;
  }
};

using namespace boost::multi_index;

template <typename Q>
using UnorderedStringViewIndexableMap = multi_index_container<
    MyPair<boost::string_view, Q>,
    indexed_by<hashed_unique<member<MyPair<boost::string_view, Q>,
                                    boost::string_view,
                                    &MyPair<boost::string_view, Q>::first>,
                             boost::hash<boost::string_view>,
                             StringViewEquals>>>;

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
    return "-" + data.to_string();
  case TokenType::identifier:
    return "identifier: " + data.to_string();
  case TokenType::arrayType:
    return "[]";
  case TokenType::filepath:
    return "filepath " + data.to_string();
  case TokenType::target_version_token:
    return data.to_string();
  case TokenType::filter_pattern:
    return "filter: " + data.to_string();
  case TokenType::eof_token:
    return "<EOF>";

  // Input/Output Options
  case TokenType::include:
    return "-include";
  case TokenType::basedirectory:
    return "-basedirectory";
  case TokenType::dump:
    return "-dump";
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
  case TokenType::returns:
    return "return";

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
    return "unknown token at line " + std::to_string(line) + " : " +
           data.to_string();
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
  case TokenType::dump:
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
  case TokenType::returns:
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

std::vector<Token> lex(const boost::string_view& in) {
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

  using TokenMap = UnorderedStringViewIndexableMap<TokenType>;

  TokenMap word_tokens{
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
      {"return", TokenType::returns},
  };

  TokenMap simple_commands{
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

  TokenMap single_filepath_commands{
      // Input/Output Options
      {"include", TokenType::include},
      {"basedirectory", TokenType::basedirectory},
      {"dump", TokenType::dump},
      {"printmapping", TokenType::printmapping},
      {"printconfiguration", TokenType::printconfiguration},
      {"printseeds", TokenType::printseeds},
      // Shrinking Options
      {"printusage", TokenType::printusage},
  };
  TokenMap multi_filepaths_commands{
      // Input/Output Options
      {"injars", TokenType::injars},
      {"outjars", TokenType::outjars},
      {"libraryjars", TokenType::libraryjars},
      // Keep Options
      {"keepdirectories", TokenType::keepdirectories},
  };

  TokenMap filter_list_commands{
      // Optimization Options
      {"optimizations", TokenType::optimizations},
      // Obfuscation Options
      {"keepattributes", TokenType::keepattributes},
      // General Options
      {"dontwarn", TokenType::dontwarn},
  };

  std::vector<Token> tokens;
  tokens.reserve(std::max((size_t)1, in.size() / 20)); // 5% ratio.

  unsigned int line = 1;

  auto add_token = [&](TokenType type) { tokens.emplace_back(type, line); };
  auto add_token_data = [&](TokenType type, const boost::string_view& data) {
    tokens.emplace_back(type, line, data);
  };
  auto add_token_line_data =
      [&](TokenType type, size_t t_line, const boost::string_view& data) {
        tokens.emplace_back(type, t_line, data);
      };

  boost::string_view data = in;
  while (!data.empty()) {
    char ch = data[0];

    // Skip comments.
    if (ch == '#') {
      auto eol_pos = data.find('\n');
      if (eol_pos != boost::string_view::npos) {
        data = data.substr(eol_pos + 1);
      } else {
        data = boost::string_view();
      }
      ++line;
      continue;
    }

    auto consume_ws = [&line, &data]() {
      size_t index = 0;
      for (; index != data.size(); ++index) {
        char c = data[index];
        if (c == '\n') {
          line++;
          continue;
        }
        if (!isspace(c)) {
          break;
        }
      }
      data = data.substr(index);
    };

    // Skip whitespaces.
    if (isspace(ch)) {
      consume_ws();
      continue;
    }

    {
      auto it = simple_tokens.find(ch);
      if (it != simple_tokens.end()) {
        add_token(it->second);
        data = data.substr(1);
        continue;
      }
    }

    if (ch == '[') {
      auto old_view = data;
      data = data.substr(1);
      consume_ws(); // Consume any whitespace
      // Check for closing brace.
      if (data.empty()) {
        add_token_data(TokenType::unknownToken, old_view);
        continue;
      }
      if (data[0] == ']') {
        add_token(TokenType::arrayType);
        data = data.substr(1);
        continue;
      }
      // Any token other than a ']' next is a bad token.
    }

    // Check for commands.
    if (ch == '-') {
      data = data.substr(1);
      auto command =
          parse_part_fn</*kSkipWs=*/false>(data, &line, is_deliminator);

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
          auto path = read_path(data, &line);
          if (!path.empty()) {
            add_token_data(TokenType::filepath, path);
          }
          continue;
        }
      }

      {
        auto it = multi_filepaths_commands.find(command);
        if (it != multi_filepaths_commands.end()) {
          add_token(it->second);
          auto paths = read_paths(data, &line);
          for (auto& path : paths) {
            add_token_line_data(TokenType::filepath, path.second, path.first);
          }
          continue;
        }
      }

      {
        auto it = filter_list_commands.find(command);
        if (it != filter_list_commands.end()) {
          add_token(it->second);
          for (auto& filter : lex_filter_list(data, &line)) {
            add_token_data(TokenType::filter_pattern, filter);
          }
          continue;
        }
      }

      // Input/Output Options
      if (command == "target") {
        add_token(TokenType::target);
        auto version = read_target_version(data, &line);
        if (!version.empty()) {
          add_token_data(TokenType::target_version_token, version);
        }
        continue;
      }

      // Obfuscation Options
      if (command == "repackageclasses") {
        add_token(TokenType::repackageclasses);
        auto package_name = parse_package_name(data, &line);
        if (!package_name.empty()) {
          add_token_data(TokenType::identifier, package_name);
        }
        continue;
      }

      // Some other command.
      add_token_data(TokenType::command, command);
      continue;
    }

    auto word = parse_part_fn</*kSkipWs=*/false>(data, &line, is_deliminator);

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
      add_token_data(TokenType::identifier, word);
      continue;
    }

    // This is an unrecognized token.
    add_token_data(TokenType::unknownToken, word);
  }
  add_token(TokenType::eof_token);
  return tokens;
}

} // namespace proguard_parser
} // namespace keep_rules
