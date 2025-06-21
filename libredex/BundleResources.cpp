/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#include <cstddef>
#include <memory>
#ifdef HAS_PROTOBUF
#include "BundleResources.h"

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/range/iterator_range.hpp>
#include <fstream>
#include <iomanip>
#include <istream>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>

#include "Debug.h"
#include "DetectBundle.h"
#include "DexUtil.h"
#include "ReadMaybeMapped.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "Trace.h"
#include "androidfw/LocaleValue.h"
#include "androidfw/ResourceTypes.h"
#include "utils/Serialize.h"

namespace {

#define MAKE_RES_ID(package, type, entry)                        \
  ((PACKAGE_MASK_BIT & ((package) << PACKAGE_INDEX_BIT_SHIFT)) | \
   (TYPE_MASK_BIT & ((type) << TYPE_INDEX_BIT_SHIFT)) |          \
   (ENTRY_MASK_BIT & (entry)))

void read_protobuf_file_contents(
    const std::string& file,
    const std::function<void(google::protobuf::io::CodedInputStream&, size_t)>&
        fn) {
  redex::read_file_with_contents(file, [&](const char* data, size_t size) {
    if (size == 0) {
      fprintf(stderr, "Unable to read protobuf file: %s\n", file.c_str());
      return;
    }
    google::protobuf::io::CodedInputStream input(
        (const google::protobuf::uint8*)data, size);
    fn(input, size);
  });
}

bool has_attribute(const aapt::pb::XmlElement& element,
                   const std::string& name) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      return true;
    }
  }
  return false;
}

bool has_primitive_attribute(const aapt::pb::XmlElement& element,
                             const std::string& name,
                             const aapt::pb::Primitive::OneofValueCase type) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      if (pb_attr.has_compiled_item()) {
        const auto& pb_item = pb_attr.compiled_item();
        if (pb_item.has_prim() && pb_item.prim().oneof_value_case() == type) {
          return true;
        }
      }
      return false;
    }
  }
  return false;
}

int get_int_attribute_value(const aapt::pb::XmlElement& element,
                            const std::string& name) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      if (pb_attr.has_compiled_item()) {
        const auto& pb_item = pb_attr.compiled_item();
        if (pb_item.has_prim() && pb_item.prim().oneof_value_case() ==
                                      aapt::pb::Primitive::kIntDecimalValue) {
          return pb_item.prim().int_decimal_value();
        }
      }
    }
  }
  throw std::runtime_error("Expected element " + element.name() +
                           " to have an int attribute " + name);
}

bool get_bool_attribute_value(const aapt::pb::XmlElement& element,
                              const std::string& name,
                              bool default_value) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      if (pb_attr.has_compiled_item()) {
        const auto& pb_item = pb_attr.compiled_item();
        if (pb_item.has_prim() && pb_item.prim().oneof_value_case() ==
                                      aapt::pb::Primitive::kBooleanValue) {
          return pb_item.prim().boolean_value();
        }
      }
      return default_value;
    }
  }
  return default_value;
}

std::string get_string_attribute_value(const aapt::pb::XmlElement& element,
                                       const std::string& name) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (pb_attr.name() == name) {
      always_assert_log(!pb_attr.has_compiled_item(),
                        "Attribute %s expected to be a string!",
                        name.c_str());
      return pb_attr.value();
    }
  }
  return std::string("");
}

boost::optional<resources::StringOrReference>
get_string_or_reference_from_attribute(const aapt::pb::XmlAttribute& pb_attr) {
  if (pb_attr.has_compiled_item()) {
    auto& item = pb_attr.compiled_item();
    // None of this was previously supported; just check for regular references
    // punting on theme refs for now.
    always_assert_log(item.has_ref(),
                      "Attribute expected to be string or a reference");
    always_assert_log(item.ref().type() ==
                          aapt::pb::Reference_Type::Reference_Type_REFERENCE,
                      "Attribute expected to be a non-theme reference");
    auto id = item.ref().id();
    if (id > 0) {
      return resources::StringOrReference(id);
    }
    return boost::none;
  } else {
    return resources::StringOrReference(pb_attr.value());
  }
}

// Apply callback to element and its descendants, stopping if/when callback
// returns false
void traverse_element_and_children(
    const aapt::pb::XmlElement& start,
    const std::function<bool(const aapt::pb::XmlElement&)>& callback) {
  std::queue<aapt::pb::XmlElement> q;
  q.push(start);
  while (!q.empty()) {
    const auto& front = q.front();
    if (!callback(front)) {
      return;
    }
    for (const aapt::pb::XmlNode& pb_child : front.child()) {
      if (pb_child.node_case() == aapt::pb::XmlNode::NodeCase::kElement) {
        q.push(pb_child.element());
      }
    }
    q.pop();
  }
}

// Look for <search_tag> within the descendants of the given XML Node
bool find_nested_tag(const std::string& search_tag,
                     const aapt::pb::XmlElement& start) {
  bool find_result = false;
  traverse_element_and_children(
      start, [&](const aapt::pb::XmlElement& element) {
        bool keep_going = true;
        if (&start != &element && element.name() == search_tag) {
          find_result = true;
          keep_going = false;
        }
        return keep_going;
      });
  return find_result;
}

void union_style_and_parent_attribute_values_impl(
    const uint32_t id,
    const std::map<uint32_t, const ConfigValues>& id_to_configvalue,
    UnorderedSet<uint32_t>* seen,
    std::vector<aapt::pb::Style::Entry>* out) {
  if (seen->count(id) > 0) {
    return;
  }
  seen->insert(id);

  auto search = id_to_configvalue.find(id);
  if (search == id_to_configvalue.end()) {
    return;
  }
  for (const auto& cv : search->second) {
    const auto& value = cv.value();
    if (value.has_compound_value() && value.compound_value().has_style()) {
      const auto& style = value.compound_value().style();
      const auto& entries = style.entry();
      for (int i = 0; i < entries.size(); i++) {
        out->push_back(entries[i]);
      }
      if (style.has_parent()) {
        auto parent_id = style.parent().id();
        union_style_and_parent_attribute_values_impl(
            parent_id, id_to_configvalue, seen, out);
      }
    }
  }
}

void union_style_and_parent_attribute_values(
    const uint32_t id,
    const std::map<uint32_t, const ConfigValues>& id_to_configvalue,
    std::vector<aapt::pb::Style::Entry>* out) {
  UnorderedSet<uint32_t> seen;
  union_style_and_parent_attribute_values_impl(id, id_to_configvalue, &seen,
                                               out);
}

// Traverse a compound value message, and return a list of Item defined in
// this message.
std::vector<aapt::pb::Item> get_items_from_compound_value(
    const aapt::pb::CompoundValue& comp_value) {
  std::vector<aapt::pb::Item> ret;
  if (comp_value.has_style()) {
    // Style style -> Entry entry -> Item item.
    const auto& entries = comp_value.style().entry();
    for (int n = 0; n < entries.size(); ++n) {
      if (entries[n].has_item()) {
        ret.push_back(entries[n].item());
      }
    }
  } else if (comp_value.has_array()) {
    // Array array -> Element element -> Item item.
    const auto& elements = comp_value.array().element();
    for (int n = 0; n < elements.size(); ++n) {
      if (elements[n].has_item()) {
        ret.push_back(elements[n].item());
      }
    }
  } else if (comp_value.has_plural()) {
    // Plural plural -> Entry entry -> Item item.
    const auto& entries = comp_value.plural().entry();
    for (int n = 0; n < entries.size(); ++n) {
      if (entries[n].has_item()) {
        ret.push_back(entries[n].item());
      }
    }
  }
  return ret;
}

// Traverse a compound value message, and return a list of Reference messages
// used in this message.
std::vector<aapt::pb::Reference> get_references(
    const aapt::pb::CompoundValue& comp_value,
    const std::vector<aapt::pb::Item>& items) {
  std::vector<aapt::pb::Reference> ret;
  // Find refs from Item message.
  for (size_t i = 0; i < items.size(); i++) {
    if (items[i].has_ref()) {
      ret.push_back(items[i].ref());
    }
  }
  // Find refs from other types of messages.
  if (comp_value.has_attr()) {
    // Attribute attr -> Symbol symbol -> Reference name.
    const auto& symbols = comp_value.attr().symbol();
    for (int i = 0; i < symbols.size(); i++) {
      if (symbols[i].has_name()) {
        ret.push_back(symbols[i].name());
      }
    }
  } else if (comp_value.has_style()) {
    // Style style -> Entry entry -> Reference key.
    const auto& entries = comp_value.style().entry();
    for (int i = 0; i < entries.size(); i++) {
      if (entries[i].has_key()) {
        ret.push_back(entries[i].key());
      }
    }
    // Style style -> Reference parent.
    if (comp_value.style().has_parent()) {
      ret.push_back(comp_value.style().parent());
    }
  } else if (comp_value.has_styleable()) {
    // Styleable styleable -> Entry entry -> Reference attr.
    const auto& entries = comp_value.styleable().entry();
    for (int i = 0; i < entries.size(); i++) {
      if (entries[i].has_attr()) {
        ret.push_back(entries[i].attr());
      }
    }
  }
  return ret;
}

void read_single_manifest(const std::string& manifest,
                          ManifestClassInfo* manifest_classes) {
  TRACE(RES, 1, "Reading proto manifest at %s", manifest.c_str());
  read_protobuf_file_contents(
      manifest,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        UnorderedMap<std::string, ComponentTag> string_to_tag{
            {"activity", ComponentTag::Activity},
            {"activity-alias", ComponentTag::ActivityAlias},
            {"provider", ComponentTag::Provider},
            {"receiver", ComponentTag::Receiver},
            {"service", ComponentTag::Service},
        };
        aapt::pb::XmlNode pb_node;

        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          manifest.c_str());
        if (pb_node.has_element() && pb_node.element().name() == "manifest") {
          const auto& manifest_element = pb_node.element();
          auto package_name =
              get_string_attribute_value(manifest_element, "package");
          traverse_element_and_children(
              manifest_element, [&](const aapt::pb::XmlElement& element) {
                const auto& tag = element.name();
                if (tag == "application") {
                  auto classname = get_string_attribute_value(element, "name");
                  if (!classname.empty()) {
                    manifest_classes->application_classes.emplace(
                        resources::fully_qualified_external_name(package_name,
                                                                 classname));
                  }
                  auto app_factory_cls = get_string_attribute_value(
                      element, "appComponentFactory");
                  if (!app_factory_cls.empty()) {
                    manifest_classes->application_classes.emplace(
                        resources::fully_qualified_external_name(
                            package_name, app_factory_cls));
                  }
                } else if (tag == "instrumentation") {
                  auto classname = get_string_attribute_value(element, "name");
                  always_assert(!classname.empty());
                  manifest_classes->instrumentation_classes.emplace(
                      resources::fully_qualified_external_name(package_name,
                                                               classname));
                } else if (string_to_tag.count(tag)) {
                  std::string classname = get_string_attribute_value(
                      element,
                      tag != "activity-alias" ? "name" : "targetActivity");
                  always_assert(!classname.empty());

                  bool has_exported_attribute = has_primitive_attribute(
                      element, "exported", aapt::pb::Primitive::kBooleanValue);
                  bool has_permission_attribute =
                      has_attribute(element, "permission");
                  bool has_protection_level_attribute =
                      has_attribute(element, "protectionLevel");
                  bool is_exported =
                      get_bool_attribute_value(element, "exported",
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
                  // NOTE: This logic is analogous to the APK manifest reading
                  // code, which is wrong. This should be a bitmask, not a
                  // string. Returning the same messed up values here to at
                  // least be consistent for now.
                  std::string permission_attribute;
                  std::string protection_level_attribute;
                  if (has_permission_attribute) {
                    permission_attribute =
                        get_string_attribute_value(element, "permission");
                  }
                  if (has_protection_level_attribute) {
                    protection_level_attribute =
                        get_string_attribute_value(element, "protectionLevel");
                  }

                  ComponentTagInfo tag_info(
                      string_to_tag.at(tag),
                      resources::fully_qualified_external_name(package_name,
                                                               classname),
                      export_attribute,
                      permission_attribute,
                      protection_level_attribute);
                  if (tag == "provider") {
                    auto text =
                        get_string_attribute_value(element, "authorities");
                    parse_authorities(text, &tag_info.authority_classes);
                  } else {
                    tag_info.has_intent_filters =
                        find_nested_tag("intent-filter", element);
                  }
                  manifest_classes->component_tags.emplace_back(tag_info);
                }
                return true;
              });
        }
      });
}

//
// PB TO ARSC CONVERSIONS
//

// Source:
// https://cs.android.com/android/platform/superproject/+/android-12.0.0_r1:frameworks/base/tools/aapt2/format/proto/ProtoDeserialize.cpp;l=68
bool DeserializeConfigFromPb(const aapt::pb::Configuration& pb_config,
                             android::ResTable_config* out_config,
                             std::string* out_error) {
  using ConfigDescription = android::ResTable_config;
  out_config->mcc = static_cast<uint16_t>(pb_config.mcc());
  out_config->mnc = static_cast<uint16_t>(pb_config.mnc());

  if (!pb_config.locale().empty()) {
    android::LocaleValue lv;
    if (!lv.InitFromBcp47Tag(pb_config.locale())) {
      std::ostringstream error;
      error << "configuration has invalid locale '" << pb_config.locale()
            << "'";
      *out_error = error.str();
      return false;
    }
    lv.WriteTo(out_config);
  }

  switch (pb_config.layout_direction()) {
  case aapt::pb::Configuration_LayoutDirection_LAYOUT_DIRECTION_LTR:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_LAYOUTDIR) |
        ConfigDescription::LAYOUTDIR_LTR;
    break;

  case aapt::pb::Configuration_LayoutDirection_LAYOUT_DIRECTION_RTL:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_LAYOUTDIR) |
        ConfigDescription::LAYOUTDIR_RTL;
    break;

  default:
    break;
  }

  out_config->smallestScreenWidthDp =
      static_cast<uint16_t>(pb_config.smallest_screen_width_dp());
  out_config->screenWidthDp =
      static_cast<uint16_t>(pb_config.screen_width_dp());
  out_config->screenHeightDp =
      static_cast<uint16_t>(pb_config.screen_height_dp());

  switch (pb_config.screen_layout_size()) {
  case aapt::pb::Configuration_ScreenLayoutSize_SCREEN_LAYOUT_SIZE_SMALL:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_SCREENSIZE) |
        ConfigDescription::SCREENSIZE_SMALL;
    break;

  case aapt::pb::Configuration_ScreenLayoutSize_SCREEN_LAYOUT_SIZE_NORMAL:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_SCREENSIZE) |
        ConfigDescription::SCREENSIZE_NORMAL;
    break;

  case aapt::pb::Configuration_ScreenLayoutSize_SCREEN_LAYOUT_SIZE_LARGE:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_SCREENSIZE) |
        ConfigDescription::SCREENSIZE_LARGE;
    break;

  case aapt::pb::Configuration_ScreenLayoutSize_SCREEN_LAYOUT_SIZE_XLARGE:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_SCREENSIZE) |
        ConfigDescription::SCREENSIZE_XLARGE;
    break;

  default:
    break;
  }

  switch (pb_config.screen_layout_long()) {
  case aapt::pb::Configuration_ScreenLayoutLong_SCREEN_LAYOUT_LONG_LONG:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_SCREENLONG) |
        ConfigDescription::SCREENLONG_YES;
    break;

  case aapt::pb::Configuration_ScreenLayoutLong_SCREEN_LAYOUT_LONG_NOTLONG:
    out_config->screenLayout =
        (out_config->screenLayout & ~ConfigDescription::MASK_SCREENLONG) |
        ConfigDescription::SCREENLONG_NO;
    break;

  default:
    break;
  }

  switch (pb_config.screen_round()) {
  case aapt::pb::Configuration_ScreenRound_SCREEN_ROUND_ROUND:
    out_config->screenLayout2 =
        (out_config->screenLayout2 & ~ConfigDescription::MASK_SCREENROUND) |
        ConfigDescription::SCREENROUND_YES;
    break;

  case aapt::pb::Configuration_ScreenRound_SCREEN_ROUND_NOTROUND:
    out_config->screenLayout2 =
        (out_config->screenLayout2 & ~ConfigDescription::MASK_SCREENROUND) |
        ConfigDescription::SCREENROUND_NO;
    break;

  default:
    break;
  }

  switch (pb_config.wide_color_gamut()) {
  case aapt::pb::Configuration_WideColorGamut_WIDE_COLOR_GAMUT_WIDECG:
    out_config->colorMode =
        (out_config->colorMode & ~ConfigDescription::MASK_WIDE_COLOR_GAMUT) |
        ConfigDescription::WIDE_COLOR_GAMUT_YES;
    break;

  case aapt::pb::Configuration_WideColorGamut_WIDE_COLOR_GAMUT_NOWIDECG:
    out_config->colorMode =
        (out_config->colorMode & ~ConfigDescription::MASK_WIDE_COLOR_GAMUT) |
        ConfigDescription::WIDE_COLOR_GAMUT_NO;
    break;

  default:
    break;
  }

  switch (pb_config.hdr()) {
  case aapt::pb::Configuration_Hdr_HDR_HIGHDR:
    out_config->colorMode =
        (out_config->colorMode & ~ConfigDescription::MASK_HDR) |
        ConfigDescription::HDR_YES;
    break;

  case aapt::pb::Configuration_Hdr_HDR_LOWDR:
    out_config->colorMode =
        (out_config->colorMode & ~ConfigDescription::MASK_HDR) |
        ConfigDescription::HDR_NO;
    break;

  default:
    break;
  }

  switch (pb_config.orientation()) {
  case aapt::pb::Configuration_Orientation_ORIENTATION_PORT:
    out_config->orientation = ConfigDescription::ORIENTATION_PORT;
    break;

  case aapt::pb::Configuration_Orientation_ORIENTATION_LAND:
    out_config->orientation = ConfigDescription::ORIENTATION_LAND;
    break;

  case aapt::pb::Configuration_Orientation_ORIENTATION_SQUARE:
    out_config->orientation = ConfigDescription::ORIENTATION_SQUARE;
    break;

  default:
    break;
  }

  switch (pb_config.ui_mode_type()) {
  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_NORMAL:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_NORMAL;
    break;

  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_DESK:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_DESK;
    break;

  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_CAR:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_CAR;
    break;

  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_TELEVISION:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_TELEVISION;
    break;

  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_APPLIANCE:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_APPLIANCE;
    break;

  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_WATCH:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_WATCH;
    break;

  case aapt::pb::Configuration_UiModeType_UI_MODE_TYPE_VRHEADSET:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_TYPE) |
        ConfigDescription::UI_MODE_TYPE_VR_HEADSET;
    break;

  default:
    break;
  }

  switch (pb_config.ui_mode_night()) {
  case aapt::pb::Configuration_UiModeNight_UI_MODE_NIGHT_NIGHT:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_NIGHT) |
        ConfigDescription::UI_MODE_NIGHT_YES;
    break;

  case aapt::pb::Configuration_UiModeNight_UI_MODE_NIGHT_NOTNIGHT:
    out_config->uiMode =
        (out_config->uiMode & ~ConfigDescription::MASK_UI_MODE_NIGHT) |
        ConfigDescription::UI_MODE_NIGHT_NO;
    break;

  default:
    break;
  }

  out_config->density = static_cast<uint16_t>(pb_config.density());

  switch (pb_config.touchscreen()) {
  case aapt::pb::Configuration_Touchscreen_TOUCHSCREEN_NOTOUCH:
    out_config->touchscreen = ConfigDescription::TOUCHSCREEN_NOTOUCH;
    break;

  case aapt::pb::Configuration_Touchscreen_TOUCHSCREEN_STYLUS:
    out_config->touchscreen = ConfigDescription::TOUCHSCREEN_STYLUS;
    break;

  case aapt::pb::Configuration_Touchscreen_TOUCHSCREEN_FINGER:
    out_config->touchscreen = ConfigDescription::TOUCHSCREEN_FINGER;
    break;

  default:
    break;
  }

  switch (pb_config.keys_hidden()) {
  case aapt::pb::Configuration_KeysHidden_KEYS_HIDDEN_KEYSEXPOSED:
    out_config->inputFlags =
        (out_config->inputFlags & ~ConfigDescription::MASK_KEYSHIDDEN) |
        ConfigDescription::KEYSHIDDEN_NO;
    break;

  case aapt::pb::Configuration_KeysHidden_KEYS_HIDDEN_KEYSHIDDEN:
    out_config->inputFlags =
        (out_config->inputFlags & ~ConfigDescription::MASK_KEYSHIDDEN) |
        ConfigDescription::KEYSHIDDEN_YES;
    break;

  case aapt::pb::Configuration_KeysHidden_KEYS_HIDDEN_KEYSSOFT:
    out_config->inputFlags =
        (out_config->inputFlags & ~ConfigDescription::MASK_KEYSHIDDEN) |
        ConfigDescription::KEYSHIDDEN_SOFT;
    break;

  default:
    break;
  }

  switch (pb_config.keyboard()) {
  case aapt::pb::Configuration_Keyboard_KEYBOARD_NOKEYS:
    out_config->keyboard = ConfigDescription::KEYBOARD_NOKEYS;
    break;

  case aapt::pb::Configuration_Keyboard_KEYBOARD_QWERTY:
    out_config->keyboard = ConfigDescription::KEYBOARD_QWERTY;
    break;

  case aapt::pb::Configuration_Keyboard_KEYBOARD_TWELVEKEY:
    out_config->keyboard = ConfigDescription::KEYBOARD_12KEY;
    break;

  default:
    break;
  }

  switch (pb_config.nav_hidden()) {
  case aapt::pb::Configuration_NavHidden_NAV_HIDDEN_NAVEXPOSED:
    out_config->inputFlags =
        (out_config->inputFlags & ~ConfigDescription::MASK_NAVHIDDEN) |
        ConfigDescription::NAVHIDDEN_NO;
    break;

  case aapt::pb::Configuration_NavHidden_NAV_HIDDEN_NAVHIDDEN:
    out_config->inputFlags =
        (out_config->inputFlags & ~ConfigDescription::MASK_NAVHIDDEN) |
        ConfigDescription::NAVHIDDEN_YES;
    break;

  default:
    break;
  }

  switch (pb_config.navigation()) {
  case aapt::pb::Configuration_Navigation_NAVIGATION_NONAV:
    out_config->navigation = ConfigDescription::NAVIGATION_NONAV;
    break;

  case aapt::pb::Configuration_Navigation_NAVIGATION_DPAD:
    out_config->navigation = ConfigDescription::NAVIGATION_DPAD;
    break;

  case aapt::pb::Configuration_Navigation_NAVIGATION_TRACKBALL:
    out_config->navigation = ConfigDescription::NAVIGATION_TRACKBALL;
    break;

  case aapt::pb::Configuration_Navigation_NAVIGATION_WHEEL:
    out_config->navigation = ConfigDescription::NAVIGATION_WHEEL;
    break;

  default:
    break;
  }

  out_config->screenWidth = static_cast<uint16_t>(pb_config.screen_width());
  out_config->screenHeight = static_cast<uint16_t>(pb_config.screen_height());
  out_config->sdkVersion = static_cast<uint16_t>(pb_config.sdk_version());
  return true;
}

//
// END PB TO ARSC CONVERSIONS
//

} // namespace

using StyleResource = resources::StyleResource;
using StyleModificationSpec = resources::StyleModificationSpec;
using ResourceAttributeMap =
    UnorderedMap<uint32_t,
                 UnorderedMap<uint32_t, StyleModificationSpec::Modification>>;

boost::optional<int32_t> BundleResources::get_min_sdk() {
  std::string base_manifest = (boost::filesystem::path(m_directory) /
                               "base/manifest/AndroidManifest.xml")
                                  .string();
  boost::optional<int32_t> result = boost::none;
  if (!boost::filesystem::exists(base_manifest)) {
    return result;
  }
  TRACE(RES, 1, "Reading proto xml at %s", base_manifest.c_str());
  read_protobuf_file_contents(
      base_manifest,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          base_manifest.c_str());
        if (pb_node.has_element()) {
          const auto& manifest_element = pb_node.element();
          for (const aapt::pb::XmlNode& pb_child : manifest_element.child()) {
            if (pb_child.node_case() == aapt::pb::XmlNode::NodeCase::kElement) {
              const auto& pb_element = pb_child.element();
              if (pb_element.name() == "uses-sdk") {
                if (has_primitive_attribute(
                        pb_element,
                        "minSdkVersion",
                        aapt::pb::Primitive::kIntDecimalValue)) {
                  result = boost::optional<int32_t>(
                      get_int_attribute_value(pb_element, "minSdkVersion"));
                  return;
                }
              }
            }
          }
        }
      });
  return result;
}

ManifestClassInfo BundleResources::get_manifest_class_info() {
  ManifestClassInfo manifest_classes;
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto manifest = entry.path() / "manifest/AndroidManifest.xml";
    if (boost::filesystem::exists(manifest)) {
      read_single_manifest(manifest.string(), &manifest_classes);
    }
  }
  return manifest_classes;
}

boost::optional<std::string> BundleResources::get_manifest_package_name() {
  std::string base_manifest = (boost::filesystem::path(m_directory) /
                               "base/manifest/AndroidManifest.xml")
                                  .string();
  boost::optional<std::string> result = boost::none;
  if (!boost::filesystem::exists(base_manifest)) {
    return result;
  }
  TRACE(RES, 1, "Reading proto xml at %s", base_manifest.c_str());
  read_protobuf_file_contents(
      base_manifest,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          base_manifest.c_str());
        if (pb_node.has_element()) {
          const auto& manifest_element = pb_node.element();
          for (const aapt::pb::XmlAttribute& pb_attr :
               manifest_element.attribute()) {
            if (pb_attr.name() == "package") {
              result = pb_attr.value();
            }
          }
        }
      });
  return result;
}

namespace {
void apply_rename_map(const std::map<std::string, std::string>& rename_map,
                      aapt::pb::XmlNode* node,
                      size_t* out_num_renamed) {
  // NOTE: The implementation that follows is not at all similar to
  // ApkResources though this is likely sufficient. ApkResources, when
  // renaming will simply iterate through a string pool, picking up anything
  // wherever it might be in the document. This is simply checking tag
  // names, attribute values and text.
  if (node->has_element()) {
    auto element = node->mutable_element();
    {
      auto search = rename_map.find(element->name());
      if (search != rename_map.end()) {
        element->set_name(search->second);
        (*out_num_renamed)++;
      }
    }
    auto attr_size = element->attribute_size();
    for (int i = 0; i < attr_size; i++) {
      auto pb_attr = element->mutable_attribute(i);
      auto search = rename_map.find(pb_attr->value());
      if (search != rename_map.end()) {
        pb_attr->set_value(search->second);
        (*out_num_renamed)++;
      }
    }
    auto child_size = element->child_size();
    for (int i = 0; i < child_size; i++) {
      auto child = element->mutable_child(i);
      apply_rename_map(rename_map, child, out_num_renamed);
    }
  } else {
    auto search = rename_map.find(node->text());
    if (search != rename_map.end()) {
      node->set_text(search->second);
      (*out_num_renamed)++;
    }
  }
}

void fully_qualify_element(
    const UnorderedMap<std::string, std::string>& element_to_class_name,
    aapt::pb::XmlNode* node,
    size_t* out_num_changed) {
  if (node->has_element()) {
    auto element = node->mutable_element();
    auto search = element_to_class_name.find(element->name());
    if (search != element_to_class_name.end()) {
      bool can_edit = true;
      auto attr_size = element->attribute_size();
      for (int i = 0; i < attr_size; i++) {
        const auto& pb_attr = element->attribute(i);
        if (pb_attr.name() == "class") {
          // this would be ambiguous if there is already a class attribute; do
          // not change this element but consider its children.
          can_edit = false;
          break;
        }
      }
      if (can_edit) {
        element->set_name("view");
        auto mutable_attributes = element->mutable_attribute();
        auto class_attribute = new aapt::pb::XmlAttribute();
        class_attribute->set_name("class");
        class_attribute->set_value(search->second);
        mutable_attributes->AddAllocated(class_attribute);
        (*out_num_changed)++;
      }
    }

    auto child_size = element->child_size();
    for (int i = 0; i < child_size; i++) {
      auto child = element->mutable_child(i);
      fully_qualify_element(element_to_class_name, child, out_num_changed);
    }
  }
}
} // namespace

bool BundleResources::rename_classes_in_layout(
    const std::string& file_path,
    const std::map<std::string, std::string>& rename_map,
    size_t* out_num_renamed) {
  bool write_failed = false;
  read_protobuf_file_contents(
      file_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          file_path.c_str());
        size_t num_renamed = 0;
        apply_rename_map(rename_map, &pb_node, &num_renamed);
        if (num_renamed > 0) {
          std::ofstream out(file_path, std::ofstream::binary);
          if (pb_node.SerializeToOstream(&out)) {
            *out_num_renamed = num_renamed;
          } else {
            write_failed = true;
          }
        }
      });
  return !write_failed;
}

void BundleResources::fully_qualify_layout(
    const UnorderedMap<std::string, std::string>& element_to_class_name,
    const std::string& file_path,
    size_t* changes) {
  read_protobuf_file_contents(
      file_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        always_assert_log(pb_node.ParseFromCodedStream(&input),
                          "BundleResoource failed to read %s",
                          file_path.c_str());
        size_t elements_changed = 0;
        fully_qualify_element(element_to_class_name, &pb_node,
                              &elements_changed);
        if (elements_changed > 0) {
          std::ofstream out(file_path, std::ofstream::binary);
          if (pb_node.SerializeToOstream(&out)) {
            *changes = elements_changed;
          }
        }
      });
}

namespace {

std::vector<std::string> find_subdirs_in_modules(
    const std::string& extracted_dir, const std::vector<std::string>& subdirs) {
  std::vector<std::string> dirs;
  boost::filesystem::path dir(extracted_dir);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    for (const auto& subdir : subdirs) {
      auto maybe = entry.path() / subdir;
      if (boost::filesystem::exists(maybe)) {
        dirs.emplace_back(maybe.string());
      }
    }
  }
  return dirs;
}

} // namespace

UnorderedSet<std::string> BundleResources::get_service_loader_classes() {
  std::vector<std::string> subdirs =
      find_subdirs_in_modules(m_directory, {"root/META-INF/services/"});

  UnorderedSet<std::string> ret_set;
  for (const auto& subdir : subdirs) {
    UnorderedSet<std::string> temp_set =
        get_service_loader_classes_helper(subdir);
    insert_unordered_iterable(ret_set, temp_set);
  }

  return ret_set;
}

std::vector<std::string> BundleResources::find_res_directories() {
  return find_subdirs_in_modules(m_directory, {"res"});
}

std::vector<std::string> BundleResources::find_lib_directories() {
  return find_subdirs_in_modules(m_directory, {"lib", "assets"});
}

std::string BundleResources::get_base_assets_dir() {
  return m_directory + "/base/assets";
}

namespace {
// Collect all resource ids referred in an given xml element.
// attr->compiled_item->ref->id
void collect_rids_for_element(const aapt::pb::XmlElement& element,
                              UnorderedSet<uint32_t>& result) {
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (!pb_attr.has_compiled_item()) {
      continue;
    }
    const auto& pb_item = pb_attr.compiled_item();
    if (pb_item.has_ref()) {
      auto rid = pb_item.ref().id();
      if (rid > PACKAGE_RESID_START) {
        result.emplace(rid);
      }
    }
  }
}

void collect_layout_classes_and_attributes_for_element(
    const aapt::pb::XmlElement& element,
    const UnorderedMap<std::string, std::string>& ns_uri_to_prefix,
    const UnorderedSet<std::string>& attributes_to_read,
    resources::StringOrReferenceSet* out_classes,
    std::unordered_multimap<std::string, resources::StringOrReference>*
        out_attributes) {
  const auto& element_name = element.name();
  // XML element could itself be a class, with classes in its attribute values.
  for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
    if (resources::POSSIBLE_CLASS_ATTRIBUTES.count(pb_attr.name()) > 0) {
      auto value = get_string_or_reference_from_attribute(pb_attr);
      if (value && value->possible_java_identifier()) {
        out_classes->emplace(*value);
      }
    }
  }

  // NOTE: xml elements that refer to application classes must have a
  // package name; elements without a package are assumed to be SDK classes
  // and will have various "android." packages prepended before
  // instantiation.
  if (resources::valid_xml_element(element_name)) {
    TRACE(RES, 9, "Considering %s as possible class in XML resource",
          element_name.c_str());
    out_classes->emplace(element_name);
  }

  if (!attributes_to_read.empty()) {
    for (const aapt::pb::XmlAttribute& pb_attr : element.attribute()) {
      const auto& attr_name = pb_attr.name();
      const auto& uri = pb_attr.namespace_uri();
      std::string fully_qualified =
          ns_uri_to_prefix.count(uri) == 0
              ? attr_name
              : (ns_uri_to_prefix.at(uri) + ":" + attr_name);
      if (attributes_to_read.count(fully_qualified) > 0) {
        auto value = get_string_or_reference_from_attribute(pb_attr);
        if (value) {
          out_attributes->emplace(fully_qualified, *value);
        }
      }
    }
  }
}

void change_resource_id_in_pb_reference(
    const std::map<uint32_t, uint32_t>& old_to_new, aapt::pb::Reference* ref) {
  auto ref_id = ref->id();
  if (old_to_new.count(ref_id)) {
    auto new_id = old_to_new.at(ref_id);
    ref->set_id(new_id);
  }
}

void change_resource_id_in_value_reference(
    const std::map<uint32_t, uint32_t>& old_to_new, aapt::pb::Value* value) {

  if (value->has_item()) {
    auto pb_item = value->mutable_item();
    if (pb_item->has_ref()) {
      change_resource_id_in_pb_reference(old_to_new, pb_item->mutable_ref());
    }
  } else if (value->has_compound_value()) {
    auto pb_compound_value = value->mutable_compound_value();
    if (pb_compound_value->has_attr()) {
      auto pb_attr = pb_compound_value->mutable_attr();
      auto symbol_size = pb_attr->symbol_size();
      for (int i = 0; i < symbol_size; ++i) {
        auto symbol = pb_attr->mutable_symbol(i);
        if (symbol->has_name()) {
          change_resource_id_in_pb_reference(old_to_new,
                                             symbol->mutable_name());
        }
      }
    } else if (pb_compound_value->has_style()) {
      auto pb_style = pb_compound_value->mutable_style();
      if (pb_style->has_parent()) {
        change_resource_id_in_pb_reference(old_to_new,
                                           pb_style->mutable_parent());
      }
      auto entry_size = pb_style->entry_size();
      for (int i = 0; i < entry_size; ++i) {
        auto entry = pb_style->mutable_entry(i);
        if (entry->has_key()) {
          change_resource_id_in_pb_reference(old_to_new, entry->mutable_key());
        }
        if (entry->has_item()) {
          auto pb_item = entry->mutable_item();
          if (pb_item->has_ref()) {
            change_resource_id_in_pb_reference(old_to_new,
                                               pb_item->mutable_ref());
          }
        }
      }
    } else if (pb_compound_value->has_styleable()) {
      auto pb_styleable = pb_compound_value->mutable_styleable();
      auto entry_size = pb_styleable->entry_size();
      for (int i = 0; i < entry_size; ++i) {
        auto entry = pb_styleable->mutable_entry(i);
        if (entry->has_attr()) {
          change_resource_id_in_pb_reference(old_to_new, entry->mutable_attr());
        }
      }
    } else if (pb_compound_value->has_array()) {
      auto pb_array = pb_compound_value->mutable_array();
      auto entry_size = pb_array->element_size();
      for (int i = 0; i < entry_size; ++i) {
        auto element = pb_array->mutable_element(i);
        if (element->has_item()) {
          auto pb_item = element->mutable_item();
          if (pb_item->has_ref()) {
            change_resource_id_in_pb_reference(old_to_new,
                                               pb_item->mutable_ref());
          }
        }
      }
    } else if (pb_compound_value->has_plural()) {
      auto pb_plural = pb_compound_value->mutable_plural();
      auto entry_size = pb_plural->entry_size();
      for (int i = 0; i < entry_size; ++i) {
        auto entry = pb_plural->mutable_entry(i);
        if (entry->has_item()) {
          auto pb_item = entry->mutable_item();
          if (pb_item->has_ref()) {
            change_resource_id_in_pb_reference(old_to_new,
                                               pb_item->mutable_ref());
          }
        }
      }
    }
  }
}

// Copy given entry to a new entry and remap id. Caller will take ownership of
// the allocated data.
aapt::pb::Entry* new_remapped_entry(
    const aapt::pb::Entry& entry,
    uint32_t res_id,
    const std::map<uint32_t, uint32_t>& old_to_new) {
  auto copy_entry = new aapt::pb::Entry(entry);
  if (old_to_new.count(res_id)) {
    uint32_t new_res_id = old_to_new.at(res_id);
    uint32_t new_entry_id = ENTRY_MASK_BIT & new_res_id;
    always_assert_log(copy_entry->has_entry_id(),
                      "Entry doesn't have id: %s",
                      copy_entry->DebugString().c_str());
    auto entry_id = copy_entry->mutable_entry_id();
    entry_id->set_id(new_entry_id);
    auto config_value_size = copy_entry->config_value_size();
    for (int i = 0; i < config_value_size; ++i) {
      auto config_value = copy_entry->mutable_config_value(i);
      always_assert_log(config_value->has_value(),
                        "ConfigValue doesn't have value: %s\nEntry:\n%s",
                        config_value->DebugString().c_str(),
                        copy_entry->DebugString().c_str());
      auto value = config_value->mutable_value();
      change_resource_id_in_value_reference(old_to_new, value);
    }
  }
  return copy_entry;
}

void remove_or_change_resource_ids(
    const UnorderedSet<uint32_t>& ids_to_remove,
    const std::map<uint32_t, uint32_t>& old_to_new,
    uint32_t package_id,
    aapt::pb::Type* type) {
  google::protobuf::RepeatedPtrField<aapt::pb::Entry> new_entries;
  for (const auto& entry : type->entry()) {
    uint32_t res_id =
        MAKE_RES_ID(package_id, type->type_id().id(), entry.entry_id().id());
    if (ids_to_remove.count(res_id)) {
      continue;
    }
    auto copy_entry = new_remapped_entry(entry, res_id, old_to_new);
    new_entries.AddAllocated(copy_entry);
  }
  type->clear_entry();
  type->mutable_entry()->Swap(&new_entries);
}

void nullify_resource_ids(const UnorderedSet<uint32_t>& ids_to_remove,
                          uint32_t package_id,
                          aapt::pb::Type* type) {
  int entry_size = type->entry_size();
  int last_non_deleted = 0;
  for (int k = 0; k < entry_size; k++) {
    auto entry = type->mutable_entry(k);
    uint32_t res_id =
        MAKE_RES_ID(package_id, type->type_id().id(), entry->entry_id().id());
    if (ids_to_remove.count(res_id)) {
      entry->clear_name();
      entry->clear_visibility();
      entry->clear_allow_new();
      entry->clear_overlayable_item();
      entry->clear_config_value();
    } else {
      last_non_deleted = k;
    }
  }
  if (last_non_deleted < entry_size - 1) {
    // Remove all entries after last_non_deleted
    type->mutable_entry()->DeleteSubrange(last_non_deleted + 1,
                                          entry_size - last_non_deleted - 1);
  }
}

void change_resource_id_in_xml_references(
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids,
    aapt::pb::XmlNode* node,
    size_t* num_resource_id_changed) {
  if (!node->has_element()) {
    return;
  }
  auto element = node->mutable_element();
  auto attr_size = element->attribute_size();
  for (int i = 0; i < attr_size; i++) {
    auto pb_attr = element->mutable_attribute(i);
    auto attr_id = pb_attr->resource_id();
    if (attr_id > 0 && kept_to_remapped_ids.count(attr_id)) {
      auto new_id = kept_to_remapped_ids.at(attr_id);
      if (new_id != attr_id) {
        (*num_resource_id_changed)++;
        pb_attr->set_resource_id(new_id);
      }
    }
    if (pb_attr->has_compiled_item()) {
      auto pb_item = pb_attr->mutable_compiled_item();
      if (pb_item->has_ref()) {
        auto ref = pb_item->mutable_ref();
        auto ref_id = ref->id();
        if (kept_to_remapped_ids.count(ref_id)) {
          auto new_id = kept_to_remapped_ids.at(ref_id);
          (*num_resource_id_changed)++;
          ref->set_id(new_id);
        }
      }
    }
  }
  auto child_size = element->child_size();
  for (int i = 0; i < child_size; i++) {
    auto child = element->mutable_child(i);
    change_resource_id_in_xml_references(kept_to_remapped_ids, child,
                                         num_resource_id_changed);
  }
}

} // namespace

void BundleResources::collect_layout_classes_and_attributes_for_file(
    const std::string& file_path,
    const UnorderedSet<std::string>& attributes_to_read,
    resources::StringOrReferenceSet* out_classes,
    std::unordered_multimap<std::string, resources::StringOrReference>*
        out_attributes) {
  if (is_raw_resource(file_path)) {
    return;
  }
  TRACE(RES,
        9,
        "BundleResources collecting classes and attributes for file: %s",
        file_path.c_str());
  read_protobuf_file_contents(
      file_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          file_path.c_str());
        if (pb_node.has_element()) {
          const auto& root = pb_node.element();
          UnorderedMap<std::string, std::string> ns_uri_to_prefix;
          for (const auto& ns_decl : root.namespace_declaration()) {
            if (!ns_decl.uri().empty() && !ns_decl.prefix().empty()) {
              ns_uri_to_prefix.emplace(ns_decl.uri(), ns_decl.prefix());
            }
          }
          traverse_element_and_children(
              root, [&](const aapt::pb::XmlElement& element) {
                collect_layout_classes_and_attributes_for_element(
                    element, ns_uri_to_prefix, attributes_to_read, out_classes,
                    out_attributes);
                return true;
              });
        }
      });
}

void BundleResources::collect_xml_attribute_string_values_for_file(
    const std::string& file_path, UnorderedSet<std::string>* out) {
  if (is_raw_resource(file_path)) {
    return;
  }
  TRACE(RES,
        9,
        "BundleResources collecting xml attribute string values for file: %s",
        file_path.c_str());
  read_protobuf_file_contents(
      file_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        always_assert_log(pb_node.ParseFromCodedStream(&input),
                          "BundleResoource failed to read %s",
                          file_path.c_str());
        if (pb_node.has_element()) {
          const auto& root = pb_node.element();
          traverse_element_and_children(
              root, [&](const aapt::pb::XmlElement& element) {
                for (const auto& pb_attr : element.attribute()) {
                  if (pb_attr.has_compiled_item()) {
                    const auto& pb_item = pb_attr.compiled_item();
                    if (pb_item.has_str()) {
                      const auto& val = pb_item.str().value();
                      if (!val.empty()) {
                        out->emplace(val);
                      }
                    } else if (pb_item.has_raw_str()) {
                      TRACE(RES, 9,
                            "Not considering %s as a possible string value",
                            pb_item.raw_str().value().c_str());
                    }
                  } else {
                    out->emplace(pb_attr.value());
                  }
                }
                return true;
              });
        }
      });
}

size_t BundleResources::remap_xml_reference_attributes(
    const std::string& filename,
    const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) {
  if (is_raw_resource(filename)) {
    return 0;
  }
  TRACE(RES,
        9,
        "BundleResources changing resource id for xml file: %s",
        filename.c_str());
  size_t num_changed = 0;
  read_protobuf_file_contents(
      filename,
      [&](google::protobuf::io::CodedInputStream& input, size_t /* unused */) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          filename.c_str());
        change_resource_id_in_xml_references(kept_to_remapped_ids, &pb_node,
                                             &num_changed);
        if (num_changed > 0) {
          std::ofstream out(filename, std::ofstream::binary);
          always_assert(pb_node.SerializeToOstream(&out));
        }
      });
  return num_changed;
}

std::vector<std::string> BundleResources::find_resources_files() {
  std::vector<std::string> paths;
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto resources_file = entry.path() / "resources.pb";
    if (boost::filesystem::exists(resources_file)) {
      paths.emplace_back(resources_file.string());
    }
  }
  return paths;
}

UnorderedSet<std::string> BundleResources::find_all_xml_files() {
  UnorderedSet<std::string> all_xml_files;
  boost::filesystem::path dir(m_directory);
  for (auto& entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    auto manifest = entry.path() / "manifest/AndroidManifest.xml";
    if (boost::filesystem::exists(manifest)) {
      all_xml_files.emplace(manifest.string());
    }
    auto res_path = entry.path() / "/res";
    insert_unordered_iterable(all_xml_files, get_xml_files(res_path.string()));
  }
  return all_xml_files;
}

UnorderedSet<uint32_t> BundleResources::get_xml_reference_attributes(
    const std::string& filename) {
  UnorderedSet<uint32_t> result;
  if (is_raw_resource(filename)) {
    return result;
  }

  read_protobuf_file_contents(
      filename,
      [&](google::protobuf::io::CodedInputStream& input, size_t size) {
        aapt::pb::XmlNode pb_node;
        bool read_finish = pb_node.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResource failed to read %s",
                          filename.c_str());
        if (pb_node.has_element()) {
          const auto& start = pb_node.element();
          traverse_element_and_children(
              start, [&](const aapt::pb::XmlElement& element) {
                collect_rids_for_element(element, result);
                return true;
              });
        }
      });
  return result;
}

void ResourcesPbFile::remap_res_ids_and_serialize(
    const std::vector<std::string>& resource_files,
    const std::map<uint32_t, uint32_t>& old_to_new) {
  for (const auto& resources_pb_path : resource_files) {
    TRACE(RES,
          9,
          "BundleResources changing resource data for file: %s",
          resources_pb_path.c_str());
    read_protobuf_file_contents(
        resources_pb_path,
        [&](google::protobuf::io::CodedInputStream& input,
            size_t /* unused */) {
          aapt::pb::ResourceTable pb_restable;
          bool read_finish = pb_restable.ParseFromCodedStream(&input);
          always_assert_log(read_finish,
                            "BundleResoource failed to read %s",
                            resources_pb_path.c_str());
          int package_size = pb_restable.package_size();
          for (int i = 0; i < package_size; i++) {
            auto package = pb_restable.mutable_package(i);
            auto current_package_id = package->package_id().id();
            int original_type_size = package->type_size();
            // Apply newly added types. Source res ids must have their data
            // remapped, according to the given map, which we will do based off
            // of the cached "ConfigValues" map.
            for (auto& type_def : m_added_types) {
              if (type_def.package_id == current_package_id) {
                TRACE(RES, 9, "Appending type %s (ID 0x%x) to package 0x%x",
                      type_def.name.c_str(), type_def.type_id,
                      type_def.package_id);
                auto new_type = package->add_type();
                new_type->set_name(type_def.name);
                new_type->mutable_type_id()->set_id(type_def.type_id);

                google::protobuf::RepeatedPtrField<aapt::pb::Entry> new_entries;
                size_t current_entry_id = 0;
                for (const auto& source_id : type_def.source_res_ids) {
                  auto& source_name = id_to_name.at(source_id);
                  auto& source_config_values =
                      m_res_id_to_configvalue.at(source_id);

                  auto source_entry = std::make_shared<aapt::pb::Entry>();
                  // Entry id needs to really just be the entry id, i.e. YYYY
                  // from 0x7fXXYYYY
                  source_entry->mutable_entry_id()->set_id(source_id & 0xFFFF);
                  source_entry->set_name(source_name);
                  source_entry->set_allocated_visibility(
                      new aapt::pb::Visibility(
                          m_res_id_to_entry.at(source_id).visibility()));
                  for (const auto& source_cv : source_config_values) {
                    auto new_config_value = source_entry->add_config_value();
                    new_config_value->set_allocated_config(
                        new aapt::pb::Configuration(source_cv.config()));
                    new_config_value->set_allocated_value(
                        new aapt::pb::Value(source_cv.value()));
                  }
                  auto remapped_entry =
                      new_remapped_entry(*source_entry, source_id, old_to_new);
                  remapped_entry->mutable_entry_id()->set_id(
                      current_entry_id++);
                  new_entries.AddAllocated(remapped_entry);
                }
                new_type->clear_entry();
                new_type->mutable_entry()->Swap(&new_entries);
              }
            }
            // Remap and apply deletions for the original types in the table.
            for (int j = 0; j < original_type_size; j++) {
              auto type = package->mutable_type(j);
              remove_or_change_resource_ids(m_ids_to_remove, old_to_new,
                                            current_package_id, type);
            }
          }
          std::ofstream out(resources_pb_path, std::ofstream::binary);
          always_assert(pb_restable.SerializeToOstream(&out));
        });
  }
}

void ResourcesPbFile::nullify_res_ids_and_serialize(
    const std::vector<std::string>& resource_files) {
  for (const auto& resources_pb_path : resource_files) {
    TRACE(RES,
          9,
          "BundleResources changing resource data for file: %s",
          resources_pb_path.c_str());
    read_protobuf_file_contents(
        resources_pb_path,
        [&](google::protobuf::io::CodedInputStream& input,
            size_t /* unused */) {
          aapt::pb::ResourceTable pb_restable;
          bool read_finish = pb_restable.ParseFromCodedStream(&input);
          always_assert_log(read_finish,
                            "BundleResoource failed to read %s",
                            resources_pb_path.c_str());
          int package_size = pb_restable.package_size();
          for (int i = 0; i < package_size; i++) {
            auto package = pb_restable.mutable_package(i);
            auto current_package_id = package->package_id().id();
            int type_size = package->type_size();
            for (int j = 0; j < type_size; j++) {
              auto type = package->mutable_type(j);
              nullify_resource_ids(m_ids_to_remove, current_package_id, type);
            }
          }
          std::ofstream out(resources_pb_path, std::ofstream::binary);
          always_assert(pb_restable.SerializeToOstream(&out));
        });
  }
}

void ResourcesPbFile::remap_reorder_and_serialize(
    const std::vector<std::string>& resource_files,
    const std::map<uint32_t, uint32_t>& old_to_new) {
  // This actually has identical implementation for .pb files.
  remap_res_ids_and_serialize(resource_files, old_to_new);
}

namespace {
void remap_entry_file_paths(const std::function<void(aapt::pb::FileReference*,
                                                     uint32_t)>& file_remapper,
                            uint32_t res_id,
                            aapt::pb::Entry* entry) {
  auto config_size = entry->config_value_size();
  for (int i = 0; i < config_size; i++) {
    auto value = entry->mutable_config_value(i)->mutable_value();
    if (value->has_item()) {
      auto item = value->mutable_item();
      if (item->has_file()) {
        auto file = item->mutable_file();
        file_remapper(file, res_id);
      }
    }
  }
}
} // namespace

void ResourcesPbFile::remap_file_paths_and_serialize(
    const std::vector<std::string>& resource_files,
    const UnorderedMap<std::string, std::string>& old_to_new) {
  auto remap_filepaths = [&old_to_new](aapt::pb::FileReference* file,
                                       uint32_t res_id) {
    auto search = old_to_new.find(file->path());
    if (search != old_to_new.end()) {
      TRACE(RES, 8, "Writing file path %s to ID 0x%x", search->second.c_str(),
            res_id);
      file->set_path(search->second);
    }
  };
  for (const auto& resources_pb_path : resource_files) {
    TRACE(RES,
          9,
          "BundleResources changing file paths for file: %s",
          resources_pb_path.c_str());
    read_protobuf_file_contents(
        resources_pb_path,
        [&](google::protobuf::io::CodedInputStream& input,
            size_t /* unused */) {
          aapt::pb::ResourceTable pb_restable;
          bool read_finish = pb_restable.ParseFromCodedStream(&input);
          always_assert_log(read_finish,
                            "BundleResoource failed to read %s",
                            resources_pb_path.c_str());
          int package_size = pb_restable.package_size();
          for (int i = 0; i < package_size; i++) {
            auto package = pb_restable.mutable_package(i);
            auto current_package_id = package->package_id().id();
            int type_size = package->type_size();
            for (int j = 0; j < type_size; j++) {
              auto type = package->mutable_type(j);
              auto current_type_id = type->type_id().id();
              int entry_size = type->entry_size();
              for (int k = 0; k < entry_size; k++) {
                auto entry = type->mutable_entry(k);
                uint32_t res_id =
                    MAKE_RES_ID(current_package_id, current_type_id,
                                entry->entry_id().id());
                remap_entry_file_paths(remap_filepaths, res_id, entry);
              }
            }
          }
          std::ofstream out(resources_pb_path, std::ofstream::binary);
          always_assert(pb_restable.SerializeToOstream(&out));
        });
  }
}

bool find_prefix_match(const UnorderedSet<std::string>& prefixes,
                       const std::string& name) {
  return unordered_any_of(
      prefixes, [&](const std::string& v) { return name.find(v) == 0; });
}

size_t ResourcesPbFile::obfuscate_resource_and_serialize(
    const std::vector<std::string>& resource_files,
    const std::map<std::string, std::string>& filepath_old_to_new,
    const UnorderedSet<uint32_t>& allowed_types,
    const UnorderedSet<std::string>& keep_resource_prefixes,
    const UnorderedSet<std::string>& keep_resource_specific) {
  if (allowed_types.empty() && filepath_old_to_new.empty()) {
    TRACE(RES, 9, "BundleResources: Nothing to change, returning");
    return 0;
  }
  size_t num_changed = 0;
  for (const auto& resources_pb_path : resource_files) {
    TRACE(RES,
          9,
          "BundleResources changing resource data for file: %s",
          resources_pb_path.c_str());
    read_protobuf_file_contents(
        resources_pb_path,
        [&](google::protobuf::io::CodedInputStream& input,
            size_t /* unused */) {
          aapt::pb::ResourceTable pb_restable;
          bool read_finish = pb_restable.ParseFromCodedStream(&input);
          always_assert_log(read_finish,
                            "BundleResoource failed to read %s",
                            resources_pb_path.c_str());
          int package_size = pb_restable.package_size();
          for (int i = 0; i < package_size; i++) {
            auto package = pb_restable.mutable_package(i);
            auto current_package_id = package->package_id().id();
            auto cur_module_name =
                resolve_module_name_for_package_id(current_package_id) + "/";
            auto remap_filepaths = [&filepath_old_to_new, &cur_module_name](
                                       aapt::pb::FileReference* file,
                                       uint32_t res_id) {
              auto search_path = cur_module_name + file->path();
              auto search = filepath_old_to_new.find(search_path);
              if (search != filepath_old_to_new.end()) {
                auto found_path = search->second;
                auto new_path = found_path.substr(cur_module_name.length());
                TRACE(RES, 8, "Writing file path %s to ID 0x%x",
                      new_path.c_str(), res_id);
                file->set_path(new_path);
              }
            };
            int type_size = package->type_size();
            for (int j = 0; j < type_size; j++) {
              auto type = package->mutable_type(j);
              auto current_type_id = type->type_id().id();
              auto is_allow_type = allowed_types.count(current_type_id) > 0;
              if (!is_allow_type && filepath_old_to_new.empty()) {
                TRACE(RES, 9,
                      "BundleResources: skipping annonymize type %X: %s",
                      current_type_id, type->name().c_str());
                continue;
              }
              int entry_size = type->entry_size();
              for (int k = 0; k < entry_size; k++) {
                auto entry = type->mutable_entry(k);
                const auto& entry_name = entry->name();
                uint32_t res_id =
                    MAKE_RES_ID(current_package_id, current_type_id,
                                entry->entry_id().id());
                remap_entry_file_paths(remap_filepaths, res_id,
                                       type->mutable_entry(k));
                if (!is_allow_type ||
                    find_prefix_match(keep_resource_prefixes, entry_name) ||
                    keep_resource_specific.count(entry_name) > 0) {
                  TRACE(RES,
                        9,
                        "BundleResources: keeping entry name %s",
                        entry_name.c_str());
                  continue;
                }
                ++num_changed;
                entry->set_name(RESOURCE_NAME_REMOVED);
              }
            }
          }
          std::ofstream out(resources_pb_path, std::ofstream::binary);
          always_assert(pb_restable.SerializeToOstream(&out));
        });
  }
  return num_changed;
}

namespace {

std::string module_name_from_pb_path(const std::string& resources_pb_path) {
  auto p = boost::filesystem::path(resources_pb_path);
  return p.parent_path().filename().string();
}

} // namespace

std::string ResourcesPbFile::resolve_module_name_for_package_id(
    uint32_t package_id) {
  always_assert_log(m_package_id_to_module_name.count(package_id) > 0,
                    "Unknown package for package id %X", package_id);
  return m_package_id_to_module_name.at(package_id);
}

std::string ResourcesPbFile::resolve_module_name_for_resource_id(
    uint32_t res_id) {
  auto package_id = res_id >> 24;
  always_assert_log(m_package_id_to_module_name.count(package_id) > 0,
                    "Unknown package for resource id %X", res_id);
  return m_package_id_to_module_name.at(package_id);
}

namespace {
void reset_pb_source(google::protobuf::Message* message) {
  if (message == nullptr) {
    return;
  }
  const google::protobuf::Descriptor* desc = message->GetDescriptor();
  const google::protobuf::Reflection* refl = message->GetReflection();
  for (int i = 0; i < desc->field_count(); i++) {
    const google::protobuf::FieldDescriptor* field_desc = desc->field(i);
    auto repeated = field_desc->is_repeated();
    auto cpp_type = field_desc->cpp_type();
    if (cpp_type == google::protobuf::FieldDescriptor::CPPTYPE_UINT32 &&
        refl->HasField(*message, field_desc) && !repeated) {
      auto name = field_desc->name();
      if (name == "path_idx" || name == "line_number" ||
          name == "column_number") {
        TRACE(RES, 9, "resetting uint32 field: %s", name.c_str());
        refl->SetUInt32(message, field_desc, 0);
      }
    } else if (cpp_type == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (repeated) {
        // Note: HasField not relevant for repeated fields.
        auto size = refl->FieldSize(*message, field_desc);
        for (int j = 0; j < size; j++) {
          auto sub_message =
              refl->MutableRepeatedMessage(message, field_desc, j);
          reset_pb_source(sub_message);
        }
      } else if (refl->HasField(*message, field_desc)) {
        auto sub_message = refl->MutableMessage(message, field_desc);
        reset_pb_source(sub_message);
      }
    }
  }
}

bool compare_reference(const aapt::pb::Reference& a,
                       const aapt::pb::Reference& b) {
  if (a.type() != b.type()) {
    return a.type() < b.type();
  }
  if (a.id() != b.id()) {
    return a.id() < b.id();
  }
  int name_compare = a.name().compare(b.name());
  if (name_compare != 0) {
    return name_compare < 0;
  }
  if (a.private_() != b.private_()) {
    return a.private_();
  }
  // Just make some kind of consistent ordering of unknown/false/true that is
  // meaningless outside this function.
  auto dynamic_to_int = [](const aapt::pb::Reference& r) {
    if (!r.has_is_dynamic()) {
      return -1;
    }
    if (!r.is_dynamic().value()) {
      return 0;
    }
    return 1;
  };
  auto da = dynamic_to_int(a);
  auto db = dynamic_to_int(b);
  if (da != db) {
    return da < db;
  }
  return false;
}

void reorder_style(aapt::pb::Style* style) {
  std::stable_sort(
      style->mutable_entry()->begin(),
      style->mutable_entry()->end(),
      [style](const aapt::pb::Style_Entry& a, const aapt::pb::Style_Entry& b) {
        always_assert_log(a.has_key() && b.has_key(),
                          "Unexpected styleable missing reference: %s",
                          style->DebugString().c_str());
        return compare_reference(a.key(), b.key());
      });
}

void reorder_config_value_repeated_field(aapt::pb::ResourceTable* pb_restable) {
  for (int package_idx = 0; package_idx < pb_restable->package_size();
       package_idx++) {
    auto package = pb_restable->mutable_package(package_idx);
    for (int type_idx = 0; type_idx < package->type_size(); type_idx++) {
      auto type = package->mutable_type(type_idx);
      for (int entry_idx = 0; entry_idx < type->entry_size(); entry_idx++) {
        auto entry = type->mutable_entry(entry_idx);
        for (int cv_idx = 0; cv_idx < entry->config_value_size(); cv_idx++) {
          auto config_value = entry->mutable_config_value(cv_idx);
          if (config_value->has_value()) {
            auto value = config_value->mutable_value();
            if (value->has_compound_value()) {
              auto compound_value = value->mutable_compound_value();
              if (compound_value->has_style()) {
                reorder_style(compound_value->mutable_style());
              }
            }
          }
        }
      }
    }
  }
}
} // namespace

void ResourcesPbFile::collect_resource_data_for_file(
    const std::string& resources_pb_path) {
  uint32_t result = 0;
  bool empty_package = true;
  TRACE(RES,
        9,
        "BundleResources collecting resource data for file: %s",
        resources_pb_path.c_str());
  read_protobuf_file_contents(
      resources_pb_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t /* unused */) {
        aapt::pb::ResourceTable pb_restable;
        bool read_finish = pb_restable.ParseFromCodedStream(&input);
        always_assert_log(read_finish, "BundleResoource failed to read %s",
                          resources_pb_path.c_str());
        if (pb_restable.has_source_pool()) {
          // Source positions refer to ResStringPool entries which are file
          // paths from the perspective of the build machine. Not relevant for
          // further operations, set them to a predictable value.
          // NOTE: Not all input .aab files will have this data; release style
          // bundles should omit this data.
          reset_pb_source(&pb_restable);
        }
        // Repeated fields might not be comming in ordered, to make following
        // config_value comparison work with different order, reorder repeated
        // fields in config_value's value
        reorder_config_value_repeated_field(&pb_restable);
        for (const aapt::pb::Package& pb_package : pb_restable.package()) {
          auto current_package_id = pb_package.package_id().id();
          if (result == 0) {
            result = current_package_id;
          } else {
            always_assert_log(
                result == current_package_id,
                "Broken assumption for only one package for resources.");
          }
          TRACE(RES, 9, "Package: %s %X", pb_package.package_name().c_str(),
                current_package_id);
          m_package_id_to_module_name.emplace(
              current_package_id, module_name_from_pb_path(resources_pb_path));
          for (const aapt::pb::Type& pb_type : pb_package.type()) {
            empty_package = false;
            auto current_type_id = pb_type.type_id().id();
            const auto& current_type_name = pb_type.name();
            TRACE(RES, 9, "  Type: %s %X", current_type_name.c_str(),
                  current_type_id);
            always_assert(m_type_id_to_names.count(current_type_id) == 0 ||
                          m_type_id_to_names.at(current_type_id) ==
                              current_type_name);
            m_type_id_to_names[current_type_id] = current_type_name;
            for (const aapt::pb::Entry& pb_entry : pb_type.entry()) {
              std::string name_string = pb_entry.name();
              auto current_entry_id = pb_entry.entry_id().id();
              auto current_resource_id = MAKE_RES_ID(
                  current_package_id, current_type_id, current_entry_id);
              TRACE(RES, 9, "    Entry: %s %X %X", pb_entry.name().c_str(),
                    current_entry_id, current_resource_id);
              sorted_res_ids.emplace_back(current_resource_id);
              always_assert(m_existed_res_ids.count(current_resource_id) == 0);
              m_existed_res_ids.emplace(current_resource_id);
              id_to_name.emplace(current_resource_id, name_string);
              name_to_ids[name_string].push_back(current_resource_id);
              m_res_id_to_entry.emplace(current_resource_id, pb_entry);
              m_res_id_to_configvalue.emplace(current_resource_id,
                                              pb_entry.config_value());
            }
            if (current_package_id == APPLICATION_PACKAGE) {
              m_application_type_ids_to_names.emplace(
                  (uint8_t)current_type_id,
                  resources::type_name_from_possibly_custom_type_name(
                      current_type_name));
            }
          }
        }
        std::sort(sorted_res_ids.begin(), sorted_res_ids.end());
      });
  if (result != 0 && !empty_package) {
    always_assert_log(m_package_ids.count(result) == 0,
                      "Redefinition of Package ID 0x%x which is unexpected",
                      result);
    m_package_ids.emplace(result);
  }
}

void ResourcesPbFile::get_type_names(std::vector<std::string>* type_names) {
  always_assert(!m_type_id_to_names.empty());
  always_assert_log(type_names->empty(),
                    "Must provide an empty vector, for documented indexing "
                    "scheme to be valid");
  auto highest_type_id = m_type_id_to_names.rbegin()->first;
  for (size_t i = 1; i <= highest_type_id; i++) {
    auto search = m_type_id_to_names.find(i);
    if (search != m_type_id_to_names.end()) {
      type_names->emplace_back(search->second);
    } else {
      type_names->emplace_back("");
    }
  }
}

UnorderedSet<uint32_t> ResourcesPbFile::get_types_by_name(
    const UnorderedSet<std::string>& type_names) {
  always_assert(!m_type_id_to_names.empty());
  UnorderedSet<uint32_t> type_ids;
  for (const auto& pair : m_type_id_to_names) {
    if (type_names.count(pair.second) == 1) {
      type_ids.emplace((pair.first) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

UnorderedSet<uint32_t> ResourcesPbFile::get_types_by_name_prefixes(
    const UnorderedSet<std::string>& type_name_prefixes) {
  always_assert(!m_type_id_to_names.empty());
  UnorderedSet<uint32_t> type_ids;
  for (const auto& pair : m_type_id_to_names) {
    const auto& type_name = pair.second;
    if (unordered_any_of(type_name_prefixes, [&](const std::string& prefix) {
          return type_name.find(prefix) == 0;
        })) {
      type_ids.emplace((pair.first) << TYPE_INDEX_BIT_SHIFT);
    }
  }
  return type_ids;
}

void ResourcesPbFile::delete_resource(uint32_t res_id) {
  // Keep track of res_id and delete later in remap_res_ids_and_serialize.
  m_ids_to_remove.emplace(res_id);
}

namespace {
const std::string KNOWN_RES_DIR = std::string(RES_DIRECTORY) + "/";

bool is_resource_file(const std::string& str) {
  return boost::algorithm::starts_with(str, KNOWN_RES_DIR);
}
} // namespace

std::vector<std::string> ResourcesPbFile::get_files_by_rid(
    uint32_t res_id, ResourcePathType path_type) {
  std::vector<std::string> ret;
  const std::string& module_name = resolve_module_name_for_resource_id(res_id);
  auto handle_path = [&](const std::string& file_path) {
    if (is_resource_file(file_path)) {
      if (path_type == ResourcePathType::ZipPath) {
        ret.emplace_back(module_name + "/" + file_path);
      } else {
        ret.emplace_back(file_path);
      }
    }
  };
  const auto& out_values = m_res_id_to_configvalue.at(res_id);
  for (auto i = 0; i < out_values.size(); i++) {
    const auto& value = out_values[i].value();
    if (value.has_item() && value.item().has_file()) {
      // Item
      const auto& file_path = value.item().file().path();
      handle_path(file_path);
    } else if (value.has_compound_value()) {
      // For compound value, we flatten it and check all its Item messages.
      const auto& items = get_items_from_compound_value(value.compound_value());
      for (size_t n = 0; n < items.size(); n++) {
        if (items[n].has_file()) {
          const auto& file_path = items[n].file().path();
          handle_path(file_path);
        }
      }
    }
  }
  return ret;
}

void ResourcesPbFile::walk_references_for_resource(
    uint32_t resID,
    const ResourcePathType& path_type,
    const resources::ReachabilityOptions& reachability_options,
    UnorderedSet<uint32_t>* nodes_visited,
    UnorderedSet<std::string>* potential_file_paths) {
  if (nodes_visited->find(resID) != nodes_visited->end() ||
      m_res_id_to_configvalue.count(resID) == 0) {
    // Return directly if a node is visited.
    return;
  }
  nodes_visited->emplace(resID);

  auto handle_item_if_file = [&](uint32_t id, const aapt::pb::Item& item) {
    if (item.has_file()) {
      if (path_type == ResourcePathType::ZipPath) {
        auto item_path =
            resolve_module_name_for_resource_id(id) + "/" + item.file().path();
        potential_file_paths->insert(item_path);
      } else {
        potential_file_paths->insert(item.file().path());
      }
    }
  };

  // For a given ID, collect any file paths and emit reachable Reference data
  // structures.
  auto collect_impl = [&](uint32_t id, std::stack<aapt::pb::Reference>* out) {
    auto res_id_search = m_res_id_to_configvalue.find(id);
    if (res_id_search == m_res_id_to_configvalue.end()) {
      // We might have some potential resource ID that does not actually exist.
      return;
    }
    auto& config_values = res_id_search->second;
    for (auto& cv : config_values) {
      auto& value = cv.value();
      std::vector<aapt::pb::Item> items;
      std::vector<aapt::pb::Reference> references;
      if (reachability_options.granular_style_reachability &&
          value.has_compound_value() && value.compound_value().has_style()) {
        std::vector<aapt::pb::Style::Entry> style_entries;
        // Resolve Style Entries up the parent hierarchy, without emitting a
        // reference to the parent itself (to disambiguate).
        union_style_and_parent_attribute_values(id, m_res_id_to_configvalue,
                                                &style_entries);
        for (auto& entry : style_entries) {
          if (entry.has_item()) {
            items.push_back(entry.item());
            if (entry.item().has_ref()) {
              references.push_back(entry.item().ref());
            }
          }
          if (entry.has_key()) {
            references.push_back(entry.key());
          }
        }
      } else if (value.has_compound_value()) {
        items = get_items_from_compound_value(value.compound_value());
        references = get_references(value.compound_value(), items);
      } else {
        items.push_back(value.item());
        if (value.item().has_ref()) {
          references.push_back(value.item().ref());
        }
      }

      // Handle items, refs
      for (auto& i : items) {
        handle_item_if_file(id, i);
      }
      for (auto& ref : references) {
        out->push(ref);
      }
    }
  };

  std::stack<aapt::pb::Reference> nodes_to_explore;
  collect_impl(resID, &nodes_to_explore);

  while (!nodes_to_explore.empty()) {
    auto reference = nodes_to_explore.top();
    nodes_to_explore.pop();

    std::vector<uint32_t> ref_ids;
    if (reference.id() != 0) {
      ref_ids.push_back(reference.id());
    } else if (!reference.name().empty()) {
      // Since id of a Reference message is optional, once ref_id =0, it is
      // possible that the resource is refered by name. If we can make sure it
      // won't happen, this branch can be removed.
      ref_ids = get_res_ids_by_name(reference.name());
    }
    for (auto ref_id : ref_ids) {
      // Skip if the node has been visited.
      if (ref_id <= PACKAGE_RESID_START ||
          nodes_visited->find(ref_id) != nodes_visited->end()) {
        continue;
      }
      nodes_visited->insert(ref_id);
      collect_impl(ref_id, &nodes_to_explore);
    }
  }
}

uint64_t ResourcesPbFile::resource_value_count(uint32_t res_id) {
  const auto& config_values = m_res_id_to_configvalue.at(res_id);
  return config_values.size();
}

namespace {
android::ResTable_config convert_to_arsc_config(
    uint32_t res_id, const aapt::pb::Configuration& pb_config) {
  std::string error_msg;
  android::ResTable_config arsc_config{};
  arsc_config.size = sizeof(android::ResTable_config);
  always_assert_log(
      DeserializeConfigFromPb(pb_config, &arsc_config, &error_msg) == true,
      "Could not convert config for ID 0x%x: %s", res_id, error_msg.c_str());
  return arsc_config;
}

bool is_value_null_or_empty(const aapt::pb::Value& pb_value) {
  if (pb_value.has_item()) {
    const auto& pb_item = pb_value.item();
    return pb_item.has_prim() && (pb_item.prim().has_empty_value() ||
                                  pb_item.prim().has_null_value());
  }
  return false;
}
} // namespace

void ResourcesPbFile::get_configurations(
    uint32_t package_id,
    const std::string& name,
    std::vector<android::ResTable_config>* configs) {
  std::set<android::ResTable_config> config_set;
  for (const auto& pair : m_type_id_to_names) {
    if (pair.second == name) {
      auto type_id = pair.first;
      for (const auto& cv_pair : m_res_id_to_configvalue) {
        auto res_id = cv_pair.first;
        if (type_id == (res_id >> TYPE_INDEX_BIT_SHIFT & 0xFF) &&
            package_id == (res_id >> PACKAGE_INDEX_BIT_SHIFT & 0xFF)) {
          for (const auto& cv : cv_pair.second) {
            auto& pb_config = cv.config();
            auto arsc_config = convert_to_arsc_config(res_id, pb_config);
            if (traceEnabled(RES, 9)) {
              auto arsc_config_string = arsc_config.toString();
              TRACE(RES, 9, "Resource ID 0x%x has value in config: %s", res_id,
                    arsc_config_string.c_str());
              std::string pb_desc;
              google::protobuf::TextFormat::PrintToString(pb_config, &pb_desc);
              TRACE(RES, 9, "  Proto config desc: %s", pb_desc.c_str());
            }
            config_set.emplace(arsc_config);
          }
        }
      }
    }
  }
  for (const auto& c : config_set) {
    configs->emplace_back(c);
  }
}

std::set<android::ResTable_config> ResourcesPbFile::get_configs_with_values(
    uint32_t id) {
  std::set<android::ResTable_config> config_set;
  auto& config_values = m_res_id_to_configvalue.at(id);
  for (const auto& cv : config_values) {
    if (cv.has_value()) {
      auto& pb_value = cv.value();
      if (!is_value_null_or_empty(pb_value)) {
        auto& pb_config = cv.config();
        auto arsc_config = convert_to_arsc_config(id, pb_config);
        config_set.emplace(arsc_config);
      }
    }
  }
  return config_set;
}

std::unique_ptr<ResourceTableFile> BundleResources::load_res_table() {
  const auto& res_pb_file_paths = find_resources_files();
  auto to_return = std::make_unique<ResourcesPbFile>(ResourcesPbFile());
  for (const auto& res_pb_file_path : res_pb_file_paths) {
    to_return->collect_resource_data_for_file(res_pb_file_path);
  }
  return to_return;
}

BundleResources::~BundleResources() {}

size_t ResourcesPbFile::get_hash_from_values(
    const ConfigValues& config_values) {
  size_t hash = 0;
  for (int i = 0; i < config_values.size(); ++i) {
    const auto& value = config_values[i].value();
    std::string value_str;
    if (value.has_item()) {
      value.item().SerializeToString(&value_str);
    } else {
      value.compound_value().SerializeToString(&value_str);
    }
    boost::hash_combine(hash, value_str);
  }
  return hash;
}

size_t ResourcesPbFile::package_count() { return m_package_ids.size(); }

void ResourcesPbFile::collect_resid_values_and_hashes(
    const std::vector<uint32_t>& ids,
    std::map<size_t, std::vector<uint32_t>>* res_by_hash) {
  for (uint32_t id : ids) {
    const auto& config_values = m_res_id_to_configvalue.at(id);
    (*res_by_hash)[get_hash_from_values(config_values)].push_back(id);
  }
}

bool ResourcesPbFile::resource_value_identical(uint32_t a_id, uint32_t b_id) {
  if ((a_id & PACKAGE_MASK_BIT) != (b_id & PACKAGE_MASK_BIT) ||
      (a_id & TYPE_MASK_BIT) != (b_id & TYPE_MASK_BIT)) {
    return false;
  }
  const auto& config_values_a = m_res_id_to_configvalue.at(a_id);
  const auto& config_values_b = m_res_id_to_configvalue.at(b_id);
  if (config_values_a.size() != config_values_b.size()) {
    return false;
  }
  // For ResTable in arsc there seems to be assumption that configuration
  // will be in same order for list of configvalues.
  // https://fburl.com/code/optgs5k3 Not sure if this will hold for protobuf
  // representation as well.
  for (int i = 0; i < config_values_a.size(); ++i) {
    const auto& config_value_a = config_values_a[i];
    const auto& config_value_b = config_values_b[i];

    const auto& config_a = config_value_a.config();
    std::string config_a_str;
    config_a.SerializeToString(&config_a_str);
    const auto& config_b = config_value_b.config();
    std::string config_b_str;
    config_b.SerializeToString(&config_b_str);
    if (config_a_str != config_b_str) {
      return false;
    }

    const auto& value_a = config_value_a.value();
    const auto& value_b = config_value_b.value();
    // Not sure if this should be compared
    if (value_a.weak() != value_b.weak()) {
      return false;
    }
    if (value_a.has_item() != value_b.has_item()) {
      return false;
    }
    std::string value_a_str;
    std::string value_b_str;
    if (value_a.has_item()) {
      value_a.item().SerializeToString(&value_a_str);
      value_b.item().SerializeToString(&value_b_str);
    } else {
      value_a.compound_value().SerializeToString(&value_a_str);
      value_b.compound_value().SerializeToString(&value_b_str);
    }
    if (value_a_str != value_b_str) {
      return false;
    }
  }
  return true;
}

namespace {

void maybe_obfuscate_element(
    const UnorderedSet<std::string>& do_not_obfuscate_elements,
    aapt::pb::XmlElement* pb_element,
    size_t* change_count) {
  if (do_not_obfuscate_elements.count(pb_element->name()) > 0) {
    return;
  }
  auto attr_count = pb_element->attribute_size();
  for (int i = 0; i < attr_count; i++) {
    auto pb_attr = pb_element->mutable_attribute(i);
    if (pb_attr->resource_id() > 0) {
      pb_attr->set_name("");
      (*change_count)++;
    }
  }
  auto child_size = pb_element->child_size();
  for (int i = 0; i < child_size; i++) {
    auto pb_child = pb_element->mutable_child(i);
    if (pb_child->has_element()) {
      auto pb_child_element = pb_child->mutable_element();
      maybe_obfuscate_element(do_not_obfuscate_elements, pb_child_element,
                              change_count);
    }
  }
}

void obfuscate_xml_attributes(
    const std::string& filename,
    const UnorderedSet<std::string>& do_not_obfuscate_elements) {
  read_protobuf_file_contents(
      filename,
      [&](google::protobuf::io::CodedInputStream& input, size_t /* unused */) {
        aapt::pb::XmlNode pb_node;
        always_assert_log(pb_node.ParseFromCodedStream(&input),
                          "BundleResource failed to read %s",
                          filename.c_str());
        size_t change_count = 0;
        if (pb_node.has_element()) {
          auto pb_element = pb_node.mutable_element();
          maybe_obfuscate_element(do_not_obfuscate_elements, pb_element,
                                  &change_count);
        }
        if (change_count > 0) {
          std::ofstream out(filename, std::ofstream::binary);
          always_assert(pb_node.SerializeToOstream(&out));
        }
      });
}
} // namespace

void BundleResources::obfuscate_xml_files(
    const UnorderedSet<std::string>& allowed_types,
    const UnorderedSet<std::string>& do_not_obfuscate_elements) {
  using path_t = boost::filesystem::path;
  using dir_iterator = boost::filesystem::directory_iterator;

  std::set<std::string> xml_paths;
  path_t dir(m_directory);
  for (auto& module_entry : boost::make_iterator_range(
           boost::filesystem::directory_iterator(dir), {})) {
    path_t res = module_entry.path() / "/res";
    if (exists(res) && is_directory(res)) {
      for (auto it = dir_iterator(res); it != dir_iterator(); ++it) {
        auto const& entry = *it;
        const path_t& entry_path = entry.path();
        const auto& entry_string = entry_path.string();
        if (is_directory(entry_path) &&
            can_obfuscate_xml_file(allowed_types, entry_string)) {
          insert_unordered_iterable(xml_paths, get_xml_files(entry_string));
        }
      }
    }
  }
  for (const auto& path : xml_paths) {
    obfuscate_xml_attributes(path, do_not_obfuscate_elements);
  }
}

void BundleResources::finalize_bundle_config(const ResourceConfig& config) {
  if (!config.canonical_entry_types.empty() && has_bundle_config(m_directory)) {
    std::string bundle_config_path =
        (boost::filesystem::path(m_directory) / "BundleConfig.pb").string();
    read_protobuf_file_contents(
        bundle_config_path,
        [&](google::protobuf::io::CodedInputStream& input, size_t size) {
          android::bundle::BundleConfig bundle_config;
          always_assert_log(bundle_config.ParseFromCodedStream(&input),
                            "BundleResource failed to read %s",
                            bundle_config_path.c_str());
          auto pb_resource_optimizations =
              bundle_config.mutable_optimizations()
                  ->mutable_resource_optimizations();
          pb_resource_optimizations->mutable_collapsed_resource_names()
              ->set_deduplicate_resource_entries(true);
          std::ofstream out(bundle_config_path, std::ofstream::binary);
          always_assert(bundle_config.SerializeToOstream(&out));
        });
  }
}

namespace {
// For the given ID, look up values in all configs for string data. Any
// references encountered will be resolved and handled recursively.
void resolve_strings_for_id(
    const std::map<uint32_t, const ConfigValues>& table_snapshot,
    uint32_t id,
    UnorderedSet<uint32_t>* seen,
    std::set<std::string>* values) {
  // Annoyingly, Android build tools allow references to have cycles in them
  // without failing at build time. At runtime, such a situation would just loop
  // a fixed number of times (https://fburl.com/xmckadjk) but we'll keep track
  // of a seen list.
  if (seen->count(id) > 0) {
    return;
  }
  seen->insert(id);
  auto search = table_snapshot.find(id);
  if (search != table_snapshot.end()) {
    auto& config_values = search->second;
    for (auto i = 0; i < config_values.size(); i++) {
      const auto& value = config_values[i].value();
      always_assert_log(value.has_item(), "Item expected for id 0x%x", id);
      auto& item = value.item();
      if (item.has_str()) {
        values->emplace(item.str().value());
      } else {
        always_assert_log(item.has_ref(),
                          "Item expected to be string or reference for id 0x%x",
                          id);
        resolve_strings_for_id(table_snapshot, item.ref().id(), seen, values);
      }
    }
  }
}
} // namespace

void ResourcesPbFile::resolve_string_values_for_resource_reference(
    uint32_t ref, std::vector<std::string>* values) {
  UnorderedSet<uint32_t> seen;
  // Ensure no duplicates
  std::set<std::string> values_set;
  resolve_strings_for_id(get_res_id_to_configvalue(), ref, &seen, &values_set);
  std::copy(values_set.begin(), values_set.end(), std::back_inserter(*values));
}

UnorderedMap<uint32_t, resources::InlinableValue>
ResourcesPbFile::get_inlinable_resource_values() {
  auto& res_id_to_configvalue = m_res_id_to_configvalue;
  UnorderedMap<uint32_t, resources::InlinableValue> inlinable_resources;
  UnorderedMap<uint32_t, uint32_t> past_refs;

  for (auto& pair : res_id_to_configvalue) {
    uint32_t id = pair.first;
    const ConfigValues& config_seq = pair.second;
    if (config_seq.empty() || config_seq.size() > 1) {
      continue;
    }
    const auto& config = config_seq.begin();
    auto arsc_config = convert_to_arsc_config(id, config->config());

    if (!arsc::is_default_config(&arsc_config)) {
      continue;
    }
    if (!config->has_value()) {
      continue;
    }
    const aapt::pb::Value& value = config->value();
    if (value.has_compound_value()) {
      continue;
    }
    const aapt::pb::Item& item = value.item();
    if (!(item.has_ref() || item.has_str() || item.has_prim())) {
      continue;
    }

    resources::InlinableValue inlinable_val{};
    if (item.has_ref()) {
      auto id_to_add = item.ref().id();
      past_refs.insert({id, id_to_add});
      continue;
    } else if (item.has_str()) {
      const std::string& og_str = item.str().value();
      const std::string& mutf8_string =
          resources::convert_utf8_to_mutf8(og_str);
      inlinable_val.string_value = mutf8_string;
      inlinable_val.type = android::Res_value::TYPE_STRING;
    } else if (item.has_prim()) {
      if (item.prim().oneof_value_case() ==
          aapt::pb::Primitive::kBooleanValue) {
        inlinable_val.bool_value = item.prim().boolean_value();
        inlinable_val.type = android::Res_value::TYPE_INT_BOOLEAN;
      } else if (item.prim().oneof_value_case() ==
                 aapt::pb::Primitive::kColorArgb4Value) {
        inlinable_val.uint_value = item.prim().color_argb4_value();
        inlinable_val.type = android::Res_value::TYPE_INT_COLOR_ARGB4;
      } else if (item.prim().oneof_value_case() ==
                 aapt::pb::Primitive::kColorArgb8Value) {
        inlinable_val.uint_value = item.prim().color_argb8_value();
        inlinable_val.type = android::Res_value::TYPE_INT_COLOR_ARGB8;
      } else if (item.prim().oneof_value_case() ==
                 aapt::pb::Primitive::kColorRgb4Value) {
        inlinable_val.uint_value = item.prim().color_rgb4_value();
        inlinable_val.type = android::Res_value::TYPE_INT_COLOR_RGB4;
      } else if (item.prim().oneof_value_case() ==
                 aapt::pb::Primitive::kColorRgb8Value) {
        inlinable_val.uint_value = item.prim().color_rgb8_value();
        inlinable_val.type = android::Res_value::TYPE_INT_COLOR_RGB8;
      } else if (item.prim().oneof_value_case() ==
                 aapt::pb::Primitive::kIntDecimalValue) {
        inlinable_val.uint_value = item.prim().int_decimal_value();
        inlinable_val.type = android::Res_value::TYPE_INT_DEC;
      } else if (item.prim().oneof_value_case() ==
                 aapt::pb::Primitive::kIntHexadecimalValue) {
        inlinable_val.uint_value = item.prim().int_hexadecimal_value();
        inlinable_val.type = android::Res_value::TYPE_INT_HEX;
      } else {
        continue;
      }
    }
    inlinable_resources.insert({id, inlinable_val});
  }
  // If a reference is found, check if the referenced value is inlinable and add
  // it's actual value to the map (instead of the reference). NOTE: only works
  // if reference only goes down one level.
  resources::resources_inlining_find_refs(past_refs, &inlinable_resources);
  return inlinable_resources;
}

UnorderedSet<uint32_t> ResourcesPbFile::get_overlayable_id_roots() {
  UnorderedSet<uint32_t> overlayable_ids;
  for (auto&& [id, entry] : m_res_id_to_entry) {
    if (entry.has_overlayable_item()) {
      overlayable_ids.emplace(id);
    }
  }
  return overlayable_ids;
}

bool has_style_value(const aapt::pb::ConfigValue& config_value) {
  return config_value.has_value() &&
         config_value.value().has_compound_value() &&
         config_value.value().compound_value().has_style();
}

std::vector<StyleResource::Value::Span> create_styled_string(
    const ::aapt::pb::Item& item) {
  std::vector<StyleResource::Value::Span> spans;
  for (const auto& span : item.styled_str().span()) {
    spans.push_back(StyleResource::Value::Span{span.tag(), span.first_char(),
                                               span.last_char()});
  }
  return spans;
}

std::pair<uint8_t, uint32_t> convert_primitive_to_res_value_data(
    const aapt::pb::Primitive& prim) {
  uint8_t data_type = android::Res_value::TYPE_NULL;
  uint32_t value = 0;

  switch (prim.oneof_value_case()) {
  case aapt::pb::Primitive::kFloatValue:
    data_type = android::Res_value::TYPE_FLOAT;
    value = prim.float_value();
    break;
  case aapt::pb::Primitive::kDimensionValue:
    data_type = android::Res_value::TYPE_DIMENSION;
    value = prim.dimension_value();
    break;
  case aapt::pb::Primitive::kFractionValue:
    data_type = android::Res_value::TYPE_FRACTION;
    value = prim.fraction_value();
    break;
  case aapt::pb::Primitive::kIntDecimalValue:
    data_type = android::Res_value::TYPE_INT_DEC;
    value = prim.int_decimal_value();
    break;
  case aapt::pb::Primitive::kIntHexadecimalValue:
    data_type = android::Res_value::TYPE_INT_HEX;
    value = prim.int_hexadecimal_value();
    break;
  case aapt::pb::Primitive::kBooleanValue:
    data_type = android::Res_value::TYPE_INT_BOOLEAN;
    value = prim.boolean_value();
    break;
  case aapt::pb::Primitive::kColorArgb4Value:
    data_type = android::Res_value::TYPE_INT_COLOR_ARGB4;
    value = prim.color_argb4_value();
    break;
  case aapt::pb::Primitive::kColorArgb8Value:
    data_type = android::Res_value::TYPE_INT_COLOR_ARGB8;
    value = prim.color_argb8_value();
    break;
  case aapt::pb::Primitive::kColorRgb4Value:
    data_type = android::Res_value::TYPE_INT_COLOR_RGB4;
    value = prim.color_rgb4_value();
    break;
  case aapt::pb::Primitive::kColorRgb8Value:
    data_type = android::Res_value::TYPE_INT_COLOR_RGB8;
    value = prim.color_rgb8_value();
    break;
  default:
    // Empty, null and deprecated values.
    break;
  }

  return {data_type, value};
}

void process_style_entry_item(uint32_t attr_id,
                              const aapt::pb::Item& item,
                              StyleResource& style_entry) {
  auto add_attribute = [&attr_id, &style_entry](uint8_t type, auto value) {
    style_entry.attributes.insert({attr_id, StyleResource::Value(type, value)});
  };

  if (item.has_ref()) {
    add_attribute(android::Res_value::TYPE_REFERENCE, item.ref().id());
  } else if (item.has_str()) {
    add_attribute(android::Res_value::TYPE_STRING, item.str().value());
  } else if (item.has_raw_str()) {
    add_attribute(android::Res_value::TYPE_STRING, item.raw_str().value());
  } else if (item.has_styled_str()) {
    const auto& styled_string = create_styled_string(item);
    add_attribute(android::Res_value::TYPE_STRING, styled_string);
  } else if (item.has_prim()) {
    const auto [data_type, value] =
        convert_primitive_to_res_value_data(item.prim());
    add_attribute(data_type, value);
  }
}

resources::StyleMap ResourcesPbFile::get_style_map() {
  resources::StyleMap style_map;

  UnorderedSet<uint8_t> style_type_ids;
  for (const auto& [type_id, name] :
       UnorderedIterable(m_application_type_ids_to_names)) {
    if (name == "style") {
      style_type_ids.emplace(type_id);
    }
  }

  for (const auto& [res_id, config_values] : m_res_id_to_configvalue) {
    uint8_t type_id = (res_id >> TYPE_INDEX_BIT_SHIFT) & 0xFF;

    if (style_type_ids.find(type_id) == style_type_ids.end()) {
      continue;
    }

    for (const auto& config_value : config_values) {
      if (!has_style_value(config_value)) {
        continue;
      }

      const auto& style = config_value.value().compound_value().style();
      StyleResource style_entry;

      style_entry.id = res_id;
      style_entry.config =
          convert_to_arsc_config(res_id, config_value.config());

      if (style.has_parent()) {
        style_entry.parent = style.parent().id();
      }

      for (const auto& entry : style.entry()) {
        if (entry.has_key() && entry.has_item()) {
          uint32_t attr_id = entry.key().id();
          process_style_entry_item(attr_id, entry.item(), style_entry);
        }
      }

      style_map[res_id].push_back(std::move(style_entry));
    }
  }

  return style_map;
}

// Finds a resource in a protocol buffer table and applies a modification to it
// The modification is applied using the user-provided callback function
// Returns true if the resource was found and modified, false otherwise
bool modify_attribute_from_style_resource(
    const ResourceAttributeMap& modifications,
    aapt::pb::ResourceTable& resource_table,
    const std::function<
        bool(aapt::pb::Style*,
             const UnorderedMap<uint32_t, StyleModificationSpec::Modification>&,
             UnorderedMap<uint32_t, StyleModificationSpec::Modification>&)>&
        modifier_function,
    ResourceAttributeMap& modified_resources) {

  constexpr const char* STYLE_TYPE_NAME = "style";
  bool is_file_modified = false;

  if (traceEnabled(RES, 9)) {
    std::string serialized;
    resource_table.SerializeToString(&serialized);
    TRACE(RES, 9, "Package count: %d", resource_table.package_size());
  }

  for (int package_idx = 0; package_idx < resource_table.package_size();
       package_idx++) {
    auto* package = resource_table.mutable_package(package_idx);
    TRACE(RES, 9, "  Package[%d]: id=0x%x name=%s", package_idx,
          package->package_id().id(), package->package_name().c_str());

    TRACE(RES, 9, "  Type count: %d", package->type_size());
    for (int type_idx = 0; type_idx < package->type_size(); type_idx++) {
      auto* resource_type = package->mutable_type(type_idx);
      if (resource_type->name() != STYLE_TYPE_NAME) {
        continue;
      }

      TRACE(RES, 9, "    Type[%d]: id=0x%x name=%s entry_count=%d", type_idx,
            resource_type->type_id().id(), resource_type->name().c_str(),
            resource_type->entry_size());

      for (int entry_idx = 0; entry_idx < resource_type->entry_size();
           entry_idx++) {
        auto* resource_entry = resource_type->mutable_entry(entry_idx);
        auto resource_id = MAKE_RES_ID(package->package_id().id(),
                                       resource_type->type_id().id(),
                                       resource_entry->entry_id().id());

        TRACE(RES, 9,
              "      Entry[%d]: id=0x%x name=%s "
              "config_value_count=%d",
              entry_idx, resource_id, resource_entry->name().c_str(),
              resource_entry->config_value_size());

        const auto& modification_pair = modifications.find(resource_id);
        if (modification_pair == modifications.end()) {
          continue;
        }

        for (int config_idx = 0;
             config_idx < resource_entry->config_value_size();
             config_idx++) {
          auto* config_value = resource_entry->mutable_config_value(config_idx);

          if (!config_value->has_value() ||
              !config_value->value().has_compound_value() ||
              !config_value->value().compound_value().has_style()) {
            continue;
          }

          auto* style = config_value->mutable_value()
                            ->mutable_compound_value()
                            ->mutable_style();
          TRACE(RES, 9, "        Style has %d attributes", style->entry_size());

          UnorderedMap<uint32_t, StyleModificationSpec::Modification>
              modified_attributes;
          is_file_modified |= modifier_function(
              style, modification_pair->second, modified_attributes);

          if (!modified_attributes.empty()) {
            modified_resources.insert({resource_id, modified_attributes});
          }
        }
      }
    }
  }

  return is_file_modified;
}

void apply_attribute_removals_for_file(
    const ResourceAttributeMap& modifications,
    const std::string& resource_path,
    const std::function<
        bool(aapt::pb::Style*,
             const UnorderedMap<uint32_t, StyleModificationSpec::Modification>&,
             UnorderedMap<uint32_t, StyleModificationSpec::Modification>&)>&
        modifier_function,
    ResourceAttributeMap& modified_resources) {
  read_protobuf_file_contents(
      resource_path,
      [&](google::protobuf::io::CodedInputStream& input, size_t /* unused */) {
        aapt::pb::ResourceTable resource_table;
        if (!resource_table.ParseFromCodedStream(&input)) {
          TRACE(RES, 9, "Failed to read resource file: %s",
                resource_path.c_str());
          return;
        }

        // Apply modifications to the resource file
        bool is_file_modified = modify_attribute_from_style_resource(
            modifications, resource_table, modifier_function,
            modified_resources);

        if (is_file_modified) {
          std::ofstream output_file(resource_path, std::ofstream::binary);
          if (!resource_table.SerializeToOstream(&output_file)) {
            TRACE(RES, 9, "Failed to write modified resource file: %s",
                  resource_path.c_str());
          }
        }
      });
}

// Assumes all modifications are being done on unambiguous resources
void ResourcesPbFile::apply_attribute_removals(
    const std::vector<StyleModificationSpec::Modification>& modifications,
    const std::vector<std::string>& resources_pb_paths) {
  ResourceAttributeMap modified_resources;

  const auto& attribute_removal_function =
      [](aapt::pb::Style* style,
         const UnorderedMap<uint32_t, StyleModificationSpec::Modification>&
             attribute_map,
         UnorderedMap<uint32_t, StyleModificationSpec::Modification>&
             modified_attributes) -> bool {
    bool removed_any = false;
    for (int attr = style->entry_size() - 1; attr >= 0; attr--) {
      auto* style_entry = style->mutable_entry(attr);
      if (style_entry->has_key()) {
        uint32_t attr_id = style_entry->key().id();
        TRACE(RES, 9, "        Attribute[%d]: id=0x%x", attr, attr_id);

        auto it = attribute_map.find(attr_id);
        if (it != attribute_map.end()) {
          style->mutable_entry()->DeleteSubrange(attr, 1);
          modified_attributes.insert({attr_id, it->second});
          removed_any = true;
        }
      }
    }
    return removed_any;
  };

  ResourceAttributeMap removals;
  for (const auto& mod : modifications) {
    if (mod.type == StyleModificationSpec::ModificationType::REMOVE_ATTRIBUTE) {

      removals[mod.resource_id].insert({mod.attribute_id.value(), mod});
    }
  }

  for (const auto& resource_path : resources_pb_paths) {
    TRACE(RES, 9, "Examining resource file: %s", resource_path.c_str());
    apply_attribute_removals_for_file(removals, resource_path,
                                      attribute_removal_function,
                                      modified_resources);
  }

  for (const auto& [resource_id, attr_map] :
       UnorderedIterable(modified_resources)) {
    for (const auto& [attr_id, mod] : UnorderedIterable(attr_map)) {
      TRACE(RES, 8, "Successfully removed attribute 0x%x from resource 0x%x",
            attr_id, resource_id);
    }
  }
}
ResourcesPbFile::~ResourcesPbFile() {}

#endif // HAS_PROTOBUF
