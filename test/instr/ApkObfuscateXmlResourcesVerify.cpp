/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <set>

#include "ApkResources.h"
#include "ObfuscateXmlVerifyHelper.h"

#include "androidfw/ResourceTypes.h"

namespace {
std::set<std::string> collect_all_attributes(const std::string& file_path) {
  std::set<std::string> results;
  auto file = RedexMappedFile::open(file_path);
  android::ResXMLTree parser;
  parser.setTo(file.const_data(), file.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + file_path);
  }
  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      auto attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; i++) {
        size_t len;
        // NOTE: .xml files in the compiled binary, at the time of writing are
        // all using UTF-8 pools. This logic is not generally portable, as some
        // files (like AndroidManifest.xml) will have their string pool entries
        // encoded as UTF-16.
        auto name_chars = parser.getAttributeName8(i, &len);
        if (name_chars != nullptr) {
          std::string name_str(name_chars);
          results.emplace(name_str);
        }
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);
  return results;
}
} // namespace

TEST_F(PostVerify, ApkObfuscateXmlResourceTest) {
  verify_kept_xml_attributes(resources, collect_all_attributes);
}
