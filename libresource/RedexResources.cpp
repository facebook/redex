/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <boost/filesystem.hpp>

#include "androidfw/ResourceTypes.h"
#include "utils/TypeHelpers.h"
#include "utils/String16.h"
#include "utils/String8.h"

#include "StringUtil.h"

constexpr size_t MIN_CLASSNAME_LENGTH = 10;
constexpr size_t MAX_CLASSNAME_LENGTH = 500;

using path_t = boost::filesystem::path;
using dir_iterator = boost::filesystem::directory_iterator;
using rdir_iterator = boost::filesystem::recursive_directory_iterator;


std::string convert_from_string16(const android::String16& string16) {
  android::String8 string8(string16);
  std::string converted(string8.string());
  return converted;
}


// Returns the attribute with the given name for the current XML element
std::string get_attribute_value(
    const android::ResXMLTree& parser,
    const android::String16& attribute_name) {

  const size_t attr_count = parser.getAttributeCount();

  for (size_t i = 0; i < attr_count; ++i) {
    size_t len;
    android::String16 key(parser.getAttributeName(i, &len));
    if (key == attribute_name) {
      const char16_t* p = parser.getAttributeStringValue(i, &len);
      if (p != nullptr) {
        android::String16 target(p, len);
        std::string converted = convert_from_string16(target);
        return converted;
      }
    }
  }
  return std::string("");
}

std::string dotname_to_dexname(const std::string& classname) {
  std::string dexname;
  dexname.reserve(classname.size() + 2);
  dexname += 'L';
  dexname += classname;
  dexname += ';';
  std::replace(dexname.begin(), dexname.end(), '.', '/');
  return dexname;
}

/*
 * Parse AndroidManifest from buffer, return a list of class names that are referenced
 */
std::unordered_set<std::string> extract_classes_from_manifest(const std::string& manifest_contents) {

  // Tags
  android::String16 activity("activity");
  android::String16 activity_alias("activity-alias");
  android::String16 application("application");
  android::String16 provider("provider");
  android::String16 receiver("receiver");
  android::String16 service("service");
  android::String16 instrumentation("instrumentation");

  // Attributes
  android::String16 authorities("authorities");
  android::String16 name("name");
  android::String16 target_activity("targetActivity");

  android::ResXMLTree parser;
  parser.setTo(manifest_contents.data(), manifest_contents.size());

  std::unordered_set<std::string> result;

  if (parser.getError() != android::NO_ERROR) {
    return result;
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      if (tag == activity ||
          tag == application ||
          tag == provider ||
          tag == receiver ||
          tag == service ||
          tag == instrumentation) {

        std::string classname = get_attribute_value(parser, name);
        if (classname.size()) {
          result.insert(dotname_to_dexname(classname));
        }

        if (tag == provider) {
          std::string text = get_attribute_value(parser, authorities);
          size_t start = 0;
          size_t end = 0;
          while ((end = text.find(';', start)) != std::string::npos) {
              result.insert(dotname_to_dexname(text.substr(start, end - start)));
              start = end + 1;
          }
          result.insert(dotname_to_dexname(text.substr(start)));
        }
      } else if (tag == activity_alias) {
        std::string classname = get_attribute_value(parser, target_activity);
        if (classname.size()) {
          result.insert(dotname_to_dexname(classname));
        }
        classname = get_attribute_value(parser, name);
        if (classname.size()) {
          result.insert(dotname_to_dexname(classname));
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
  return result;
}


std::unordered_set<std::string> extract_classes_from_layout(const std::string& layout_contents) {

  android::ResXMLTree parser;
  parser.setTo(layout_contents.data(), layout_contents.size());

  std::unordered_set<std::string> result;

  android::String16 name("name");
  android::String16 klazz("class");

  if (parser.getError() != android::NO_ERROR) {
    return result;
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      std::string classname = convert_from_string16(tag);
      if (!strcmp(classname.c_str(), "fragment") || !strcmp(classname.c_str(), "view")) {
        classname = get_attribute_value(parser, klazz);
        if (classname.empty()) {
          classname = get_attribute_value(parser, name);
        }
      }
      std::string converted = std::string("L") + classname + std::string(";");

      bool is_classname = converted.find('.') != std::string::npos;
      if (is_classname) {
        std::replace(converted.begin(), converted.end(), '.', '/');
        result.insert(converted);
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
  return result;
}


/*
 * Returns all strings that look like java class names from a native library.
 *
 * Return values will be formatted the way that the dex spec formats class names:
 *
 *   "Ljava/lang/String;"
 *
 */
std::unordered_set<std::string> extract_classes_from_native_lib(const std::string& lib_contents) {
  std::unordered_set<std::string> classes;
  char buffer[MAX_CLASSNAME_LENGTH + 2]; // +2 for the trailing ";\0"
  const char* inptr = lib_contents.data();
  char* outptr = buffer;
  const char* end = inptr + lib_contents.size();

  size_t length = 0;

  while (inptr < end) {
    outptr = buffer;
    length = 0;
    // All classnames start with a package, which starts with a lowercase letter. Some of them are
    // preceded by an 'L' and followed by a ';' in native libraries while others are not.
    if ((*inptr >= 'a' && *inptr <= 'z') || *inptr == 'L') {

      if (*inptr != 'L') {
        *outptr++ = 'L';
        length++;
      }

      while (( // This loop is safe since lib_contents.data() ends with a \0
          (*inptr >= 'a' && *inptr <= 'z') ||
          (*inptr >= 'A' && *inptr <= 'Z') ||
          (*inptr >= '0' && *inptr <= '9') ||
          *inptr == '/' || *inptr == '_' || *inptr == '$')
          && length < MAX_CLASSNAME_LENGTH) {

        *outptr++ = *inptr++;
        length++;
      }
      if (length >= MIN_CLASSNAME_LENGTH) {
        *outptr++ = ';';
        *outptr = '\0';
        classes.insert(std::string(buffer));
      }
    }
    inptr++;
  }
  return classes;
}


/*
 * Reads an entire file into a std::string. Returns an empty string if anything
 * went wrong (e.g. file not found).
 */
std::string read_entire_file(const std::string& filename) {
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  std::stringstream sstr;
  sstr << in.rdbuf();
  return sstr.str();
}


std::unordered_set<std::string> get_manifest_classes(const std::string& filename) {
  std::string manifest = read_entire_file(filename);
  std::unordered_set<std::string> classes;
  if (manifest.size()) {
    classes = extract_classes_from_manifest(manifest);
  } else {
    fprintf(stderr, "Unable to read manifest file: %s\n", filename.data());
  }
  return classes;
}


std::vector<std::string> find_layout_files(const std::string& apk_directory) {

  std::vector<std::string> layout_files;

  std::string root = apk_directory + std::string("/res");
  path_t res(root);

  if (exists(res) && is_directory(res)) {
    for (auto it = dir_iterator(res); it != dir_iterator(); ++it) {
      auto const& entry = *it;
      path_t entry_path = entry.path();

      if (is_directory(entry_path) &&
        starts_with(entry_path.filename().string().c_str(), "layout")) {
        for (auto lit = dir_iterator(entry_path); lit != dir_iterator(); ++lit) {
          auto const& layout_entry = *lit;
          path_t layout_path = layout_entry.path();
          if (is_regular_file(layout_path)) {
            layout_files.push_back(layout_path.string());
          }
        }
      }
    }
  }
  return layout_files;
}

std::unordered_set<std::string> get_layout_classes(const std::string& apk_directory) {
  std::vector<std::string> tmp = find_layout_files(apk_directory);
  std::unordered_set<std::string> all_classes;
  for (auto layout_file : tmp) {
    std::string contents = read_entire_file(layout_file);
    std::unordered_set<std::string> classes_from_layout = extract_classes_from_layout(contents);
    all_classes.insert(classes_from_layout.begin(), classes_from_layout.end());
  }
  return all_classes;
}


/**
 * Return a list of all the .so files in /lib
 */
std::vector<std::string> find_native_library_files(const std::string& apk_directory) {
  std::vector<std::string> native_library_files;
  std::string lib_root = apk_directory + std::string("/lib");
  std::string library_extension(".so");

  path_t lib(lib_root);

  if (exists(lib) && is_directory(lib)) {
    for (auto it = rdir_iterator(lib); it != rdir_iterator(); ++it) {
      auto const& entry = *it;
      path_t entry_path = entry.path();
      if (is_regular_file(entry_path) &&
          ends_with(entry_path.filename().string().c_str(),
                    library_extension.c_str())) {
        native_library_files.push_back(entry_path.string());
      }
    }
  }
  return native_library_files;
}


/**
 * Return all potential java class names located in native libraries.
 */
std::unordered_set<std::string> get_native_classes(const std::string& apk_directory) {
  std::vector<std::string> native_libs = find_native_library_files(apk_directory);
  std::unordered_set<std::string> all_classes;
  for (auto native_lib : native_libs) {
    std::string contents = read_entire_file(native_lib);
    std::unordered_set<std::string> classes_from_layout = extract_classes_from_native_lib(contents);
    all_classes.insert(classes_from_layout.begin(), classes_from_layout.end());
  }
  return all_classes;
}
