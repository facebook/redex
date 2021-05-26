/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApkResources.h"

#include "RedexMappedFile.h"
#include "RedexResources.h"
#include <boost/optional.hpp>

// Workaround for inclusion order, when compiling on Windows (#defines NO_ERROR
// as 0).
#ifdef NO_ERROR
#undef NO_ERROR
#endif

boost::optional<int32_t> ApkResources::get_min_sdk() {
  auto file = RedexMappedFile::open(m_manifest);

  if (file.size() == 0) {
    fprintf(stderr, "WARNING: Cannot find/read the manifest file %s\n",
            m_manifest.c_str());
    return boost::none;
  }

  android::ResXMLTree parser;
  parser.setTo(file.const_data(), file.size());

  if (parser.getError() != android::NO_ERROR) {
    fprintf(stderr, "WARNING: Failed to parse the manifest file %s\n",
            m_manifest.c_str());
    return boost::none;
  }

  const android::String16 uses_sdk("uses-sdk");
  const android::String16 min_sdk("minSdkVersion");
  android::ResXMLParser::event_code_t event_code;
  do {
    event_code = parser.next();
    if (event_code == android::ResXMLParser::START_TAG) {
      size_t outLen;
      auto el_name = android::String16(parser.getElementName(&outLen));
      if (el_name == uses_sdk) {
        android::Res_value raw_value;
        if (has_raw_attribute_value(parser, min_sdk, raw_value) &&
            (raw_value.dataType & android::Res_value::TYPE_INT_DEC)) {
          return boost::optional<int32_t>(static_cast<int32_t>(raw_value.data));
        } else {
          return boost::none;
        }
      }
    }
  } while ((event_code != android::ResXMLParser::END_DOCUMENT) &&
           (event_code != android::ResXMLParser::BAD_DOCUMENT));
  return boost::none;
}
