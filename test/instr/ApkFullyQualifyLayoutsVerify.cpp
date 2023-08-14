/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <set>

#include "ApkResources.h"
#include "FullyQualifyLayoutsVerifyHelper.h"
#include "androidfw/ResourceTypes.h"

TEST_F(PostVerify, ApkFullyQualifyLayoutsTest) {
  auto file_path = resources.at("res/layout/test_views.xml");
  auto file = RedexMappedFile::open(file_path);
  android::ResXMLTree parser;
  parser.setTo(file.const_data(), file.size());
  if (parser.getError() != android::NO_ERROR) {
    throw std::runtime_error("Unable to read file: " + file_path);
  }

  // Parse the nodes, flatten them to a vector and just capture only the
  // attributes/values we really care about for validation purposes.
  std::vector<Element> elements;
  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 name(parser.getElementName(&len));
      Element element;
      element.name = convert_from_string16(name);
      auto attr_count = parser.getAttributeCount();
      for (size_t i = 0; i < attr_count; i++) {
        android::String16 attr_name(parser.getAttributeName(i, &len));
        auto attr_name_str = convert_from_string16(attr_name);
        if (attr_name_str == "class") {
          android::String16 value(parser.getAttributeStringValue(i, &len));
          element.string_attributes.emplace(attr_name_str,
                                            convert_from_string16(value));
        }
      }
      elements.emplace_back(element);
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  verify_xml_element_attributes(elements);
}
