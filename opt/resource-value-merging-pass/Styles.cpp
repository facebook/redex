/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Styles.h"

#include "Trace.h"
#include "utils/Serialize.h"

#define NON_AMBIGUOUS(style_vec) (*(style_vec).begin())

bool is_style_ambiguous(uint32_t id,
                        resources::StyleInfo& style_info,
                        UnorderedSet<uint32_t>* ambiguous) {
  if (ambiguous->count(id) > 0) {
    return true;
  }
  auto search = style_info.styles.find(id);
  if (search != style_info.styles.end()) {
    auto& styles_vec = search->second;
    auto size = styles_vec.size();
    if (size > 1 || size == 0 ||
        !arsc::is_default_config(&styles_vec.begin()->config)) {
      ambiguous->emplace(id);
      return true;
    }
    auto& style = NON_AMBIGUOUS(styles_vec);
    if (style.parent == 0) {
      return false;
    } else {
      return is_style_ambiguous(style.parent, style_info, ambiguous);
    }
  }
  // This could be an assert, but for now just signal that an unknown ID is
  // ambiguous.
  return false;
}

UnorderedSet<uint32_t> StyleAnalysis::directly_reachable_styles() {
  if (!m_directly_reachable_styles.has_value()) {
    m_directly_reachable_styles =
        m_reachable_resources->compute_transitive_closure(m_roots);
  }

  return m_directly_reachable_styles.value();
}

UnorderedSet<uint32_t> StyleAnalysis::ambiguous_styles() {
  UnorderedSet<uint32_t> ambiguous;
  for (auto&& [id, _] : m_style_info.styles) {
    if (is_style_ambiguous(id, m_style_info, &ambiguous)) {
      TRACE(RES, 3, "Note: 0x%x is ambiguous", id);
      ambiguous.emplace(id);
    }
  }
  return ambiguous;
}

void print_attributes(std::ostringstream& oss,
                      const resources::StyleResource& style) {
  if (!style.attributes.empty()) {
    oss << "\\nAttributes:";
    for (const auto& [attr_id, _] : style.attributes) {
      oss << "\\n 0x" << std::hex << attr_id << " ";
    }
  }
}

std::string StyleAnalysis::dot(bool exclude_nodes_with_no_edges,
                               bool display_attributes) {
  const auto& directly_reachable = directly_reachable_styles();
  auto ambiguous = ambiguous_styles();
  auto& id_to_name = m_reachable_resources->get_res_table()->id_to_name;
  UnorderedMap<uint32_t, UnorderedMap<std::string, std::string>> node_options;
  for (auto id : UnorderedIterable(directly_reachable)) {
    UnorderedMap<std::string, std::string> options{{"fillcolor", "yellow"},
                                                   {"style", "filled"}};
    node_options.emplace(id, options);
  }
  for (auto id : UnorderedIterable(ambiguous)) {
    node_options[id].emplace("fillcolor", "grey");
    node_options[id].emplace("style", "filled");
  }
  auto stringify = [&](uint32_t id) {
    std::ostringstream oss;
    const auto& res_name = id_to_name.at(id);
    oss << "0x" << std::hex << id << std::dec << " " << res_name;
    auto& vec = m_style_info.styles.at(id);
    auto implementations = vec.size();
    if (implementations > 1) {
      oss << "\\nAMBIGUOUS (implementations = " << implementations << ")";
      if (display_attributes) {
        for (auto& style : vec) {
          print_attributes(oss, style);
        }
      }
    } else if (implementations == 1) {
      auto& style = vec.at(0);
      oss << "\\n(attr count = " << style.attributes.size() << ")";
      if (display_attributes) {
        print_attributes(oss, style);
      }
    } else {
      oss << "\\nEMPTY";
    }
    return oss.str();
  };
  return m_style_info.print_as_dot(
      stringify, node_options, exclude_nodes_with_no_edges);
}
