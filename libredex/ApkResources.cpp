/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApkResources.h"

#include "RedexMappedFile.h"
#include "RedexResources.h"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/TypeHelpers.h"

#include "Debug.h"
#include "DexUtil.h"
#include "IOUtil.h"
#include "Macros.h"
#include "ReadMaybeMapped.h"

// Workaround for inclusion order, when compiling on Windows (#defines NO_ERROR
// as 0).
#ifdef NO_ERROR
#undef NO_ERROR
#endif

namespace {
/*
 * Look for <search_tag> within the descendants of the current node in the XML
 * tree.
 */
bool find_nested_tag(const android::String16& search_tag,
                     android::ResXMLTree* parser) {
  size_t depth{1};
  while (depth) {
    auto type = parser->next();
    switch (type) {
    case android::ResXMLParser::START_TAG: {
      ++depth;
      size_t len;
      android::String16 tag(parser->getElementName(&len));
      if (tag == search_tag) {
        return true;
      }
      break;
    }
    case android::ResXMLParser::END_TAG: {
      --depth;
      break;
    }
    case android::ResXMLParser::BAD_DOCUMENT: {
      not_reached();
    }
    default: {
      break;
    }
    }
  }
  return false;
}

/*
 * Parse AndroidManifest from buffer, return a list of class names that are
 * referenced
 */
ManifestClassInfo extract_classes_from_manifest(const char* data, size_t size) {
  // Tags
  android::String16 activity("activity");
  android::String16 activity_alias("activity-alias");
  android::String16 application("application");
  android::String16 provider("provider");
  android::String16 receiver("receiver");
  android::String16 service("service");
  android::String16 instrumentation("instrumentation");
  android::String16 intent_filter("intent-filter");

  // This is not an unordered_map because String16 doesn't define a hash
  std::map<android::String16, ComponentTag> string_to_tag{
      {activity, ComponentTag::Activity},
      {activity_alias, ComponentTag::ActivityAlias},
      {provider, ComponentTag::Provider},
      {receiver, ComponentTag::Receiver},
      {service, ComponentTag::Service},
  };

  // Attributes
  android::String16 authorities("authorities");
  android::String16 exported("exported");
  android::String16 protection_level("protectionLevel");
  android::String16 permission("permission");
  android::String16 name("name");
  android::String16 target_activity("targetActivity");
  android::String16 app_component_factory("appComponentFactory");

  android::ResXMLTree parser;
  parser.setTo(data, size);

  ManifestClassInfo manifest_classes;

  if (parser.getError() != android::NO_ERROR) {
    return manifest_classes;
  }

  android::ResXMLParser::event_code_t type;
  do {
    type = parser.next();
    if (type == android::ResXMLParser::START_TAG) {
      size_t len;
      android::String16 tag(parser.getElementName(&len));
      if (tag == application) {
        std::string classname = get_string_attribute_value(parser, name);
        // android:name is an optional attribute for <application>
        if (!classname.empty()) {
          manifest_classes.application_classes.emplace(
              java_names::external_to_internal(classname));
        }
        std::string app_factory_cls =
            get_string_attribute_value(parser, app_component_factory);
        if (!app_factory_cls.empty()) {
          manifest_classes.application_classes.emplace(
              java_names::external_to_internal(app_factory_cls));
        }
      } else if (tag == instrumentation) {
        std::string classname = get_string_attribute_value(parser, name);
        always_assert(classname.size());
        manifest_classes.instrumentation_classes.emplace(
            java_names::external_to_internal(classname));
      } else if (string_to_tag.count(tag)) {
        std::string classname = get_string_attribute_value(
            parser, tag != activity_alias ? name : target_activity);
        always_assert(classname.size());

        bool has_exported_attribute = has_bool_attribute(parser, exported);
        android::Res_value ignore_output;
        bool has_permission_attribute =
            has_raw_attribute_value(parser, permission, ignore_output);
        bool has_protection_level_attribute =
            has_raw_attribute_value(parser, protection_level, ignore_output);
        bool is_exported = get_bool_attribute_value(parser, exported,
                                                    /* default_value */ false);

        BooleanXMLAttribute export_attribute;
        if (has_exported_attribute) {
          if (is_exported) {
            export_attribute = BooleanXMLAttribute::True;
          } else {
            export_attribute = BooleanXMLAttribute::False;
          }
        } else {
          export_attribute = BooleanXMLAttribute::Undefined;
        }
        std::string permission_attribute;
        std::string protection_level_attribute;
        if (has_permission_attribute) {
          permission_attribute = get_string_attribute_value(parser, permission);
        }
        if (has_protection_level_attribute) {
          protection_level_attribute =
              get_string_attribute_value(parser, protection_level);
        }

        ComponentTagInfo tag_info(string_to_tag.at(tag),
                                  java_names::external_to_internal(classname),
                                  export_attribute,
                                  permission_attribute,
                                  protection_level_attribute);

        if (tag == provider) {
          std::string text = get_string_attribute_value(parser, authorities);
          size_t start = 0;
          size_t end = 0;
          while ((end = text.find(';', start)) != std::string::npos) {
            tag_info.authority_classes.insert(java_names::external_to_internal(
                text.substr(start, end - start)));
            start = end + 1;
          }
          tag_info.authority_classes.insert(
              java_names::external_to_internal(text.substr(start)));
        } else {
          tag_info.has_intent_filters = find_nested_tag(intent_filter, &parser);
        }

        manifest_classes.component_tags.emplace_back(tag_info);
      }
    }
  } while (type != android::ResXMLParser::BAD_DOCUMENT &&
           type != android::ResXMLParser::END_DOCUMENT);

  return manifest_classes;
}
} // namespace

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

ManifestClassInfo ApkResources::get_manifest_class_info() {
  std::string manifest =
      (boost::filesystem::path(m_directory) / "AndroidManifest.xml").string();
  ManifestClassInfo classes;
  if (boost::filesystem::exists(manifest)) {
    redex::read_file_with_contents(manifest, [&](const char* data,
                                                 size_t size) {
      if (size == 0) {
        fprintf(stderr, "Unable to read manifest file: %s\n", manifest.c_str());
        return;
      }
      classes = extract_classes_from_manifest(data, size);
    });
  }
  return classes;
}
