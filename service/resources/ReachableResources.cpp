/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReachableResources.h"

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex.hpp>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "RedexResources.h"
#include "StlUtil.h"
#include "Walkers.h"

namespace {
// Return true if the given string is a relative file path, has .xml extension
// and can refer to the res directory of an .apk or .aab file.
bool is_resource_xml(const std::string& str) {
  if (boost::algorithm::ends_with(str, ".xml")) {
    boost::filesystem::path p(str);
    if (p.is_relative()) {
      auto it = p.begin();
      if (it->string() == RES_DIRECTORY) {
        return true;
      }
      if (it != p.end() && (++it)->string() == RES_DIRECTORY) {
        return true;
      }
    }
  }
  return false;
}

std::unordered_set<uint32_t> get_disallowed_resources(
    const std::vector<uint32_t>& sorted_res_ids,
    const std::unordered_set<uint32_t>& disallowed_types) {
  std::unordered_set<uint32_t> disallowed_resources;
  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    uint32_t id = sorted_res_ids[index];
    uint32_t type_id = id & TYPE_MASK_BIT;
    if (disallowed_types.count(type_id)) {
      disallowed_resources.emplace(id);
    }
  }
  return disallowed_resources;
}

std::unordered_set<uint32_t> get_resources_by_name_prefix(
    const std::vector<std::string>& prefixes,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids) {
  std::unordered_set<uint32_t> found_resources;
  for (const auto& pair : name_to_ids) {
    for (const auto& prefix : prefixes) {
      if (boost::algorithm::starts_with(pair.first, prefix)) {
        found_resources.insert(pair.second.begin(), pair.second.end());
      }
    }
  }
  return found_resources;
}

std::unordered_set<uint32_t> find_code_resource_references(
    DexStoresVector& stores,
    const resources::RClassReader& r_class_reader,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids,
    bool check_string_for_name,
    bool assume_id_inlined) {
  std::unordered_set<uint32_t> ids_from_code;
  Scope scope = build_class_scope(stores);
  ConcurrentSet<uint32_t> potential_ids_from_code;
  ConcurrentSet<DexField*> accessed_sfields;
  ConcurrentSet<uint32_t> potential_ids_from_strings;
  boost::regex find_ints("(\\d+)");

  walk::parallel::opcodes(scope, [&](DexMethod* m, IRInstruction* insn) {
    // Collect all accessed fields that could be R fields, or values that got
    // inlined elsewhere.
    if (insn->has_field() && opcode::is_an_sfield_op(insn->opcode())) {
      auto field = resolve_field(insn->get_field(), FieldSearch::Static);
      if (field && field->is_concrete()) {
        accessed_sfields.emplace(field);
      }
    } else if (insn->has_literal()) {
      auto lit = insn->get_literal();
      if (assume_id_inlined && resources::is_potential_resid(lit)) {
        potential_ids_from_code.emplace(lit);
      }
    } else if (insn->has_string()) {
      std::string to_find = insn->get_string()->str_copy();
      if (assume_id_inlined) {
        // Redex evaluates expressions like
        // String.valueOf(R.drawable.inspiration_no_format)
        // which means we need to parse ints encoded as strings or ints that
        // were constant folded/concatenated at build time with other strings.
        std::vector<std::string> int_strings;
        int_strings.insert(int_strings.end(),
                           boost::sregex_token_iterator(
                               to_find.begin(), to_find.end(), find_ints),
                           boost::sregex_token_iterator());
        for (const auto& int_string : int_strings) {
          int64_t potential_num;
          try {
            potential_num = std::stol(int_string);
          } catch (...) {
            continue;
          }
          if (resources::is_potential_resid(potential_num)) {
            potential_ids_from_code.emplace(potential_num);
          }
        }
      }
      if (check_string_for_name) {
        // Being more conservative of what might get passed into
        // Landroid/content/res/Resources;.getIdentifier:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I
        auto search = name_to_ids.find(to_find);
        if (search != name_to_ids.end()) {
          potential_ids_from_strings.insert(search->second.begin(),
                                            search->second.end());
        }
      }
    } else if (assume_id_inlined && insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      auto op_data = insn->get_data();
      auto cls = type_class(m->get_class());
      // Do not blanket assume the filling of customized arrays is a usage.
      auto customized_r = !resources::is_non_customized_r_class(cls) &&
                          r_class_reader.is_r_class(cls);
      if (!customized_r && fill_array_data_payload_width(op_data) == 4) {
        // Consider only int[] for resource ids.
        auto payload = get_fill_array_data_payload<uint32_t>(op_data);
        for (const auto& lit : payload) {
          if (resources::is_potential_resid(lit)) {
            potential_ids_from_code.emplace(lit);
          }
        }
      }
    }
  });

  std::unordered_set<DexField*> array_fields;
  for (auto* field : accessed_sfields) {
    auto is_r_field =
        resources::is_non_customized_r_class(type_class(field->get_class()));
    if (type::is_primitive(field->get_type()) && field->get_static_value() &&
        resources::is_potential_resid(field->get_static_value()->value()) &&
        (is_r_field || assume_id_inlined)) {
      ids_from_code.emplace(field->get_static_value()->value());
    } else if (is_r_field && type::is_array(field->get_type())) {
      array_fields.emplace(field);
    }
  }

  r_class_reader.extract_resource_ids_from_static_arrays(scope, array_fields,
                                                         &ids_from_code);
  ids_from_code.insert(potential_ids_from_code.begin(),
                       potential_ids_from_code.end());
  ids_from_code.insert(potential_ids_from_strings.begin(),
                       potential_ids_from_strings.end());
  return ids_from_code;
}
} // namespace

namespace resources {
std::unordered_set<uint32_t> ReachableResources::get_resource_roots(
    DexStoresVector& stores) {
  // Roots from dex code.
  auto ids_from_code = find_code_resource_references(
      stores, *m_r_class_reader, m_res_table->name_to_ids,
      m_options.check_string_for_name, m_options.assume_id_inlined);
  const auto& sorted_res_ids = m_res_table->sorted_res_ids;
  std::unordered_set<uint32_t> existing_resids;
  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    existing_resids.emplace(sorted_res_ids[index]);
  }
  std20::erase_if(ids_from_code, [&](const auto& resid) {
    return existing_resids.find(resid) == existing_resids.end();
  });
  m_code_roots = ids_from_code.size();

  // Roots from AndroidManifest.xml files.
  std::unordered_set<uint32_t> manifest_roots;
  const auto& xml_files = m_resources->find_all_xml_files();
  for (const std::string& path : xml_files) {
    if (path.find("AndroidManifest.xml") == std::string::npos) {
      continue;
    }
    m_explored_xml_files.emplace(path);
    const auto& id_roots = m_resources->get_xml_reference_attributes(path);
    manifest_roots.insert(id_roots.begin(), id_roots.end());
  }
  m_manifest_roots = manifest_roots.size();

  // Configured assumptions.
  auto assumed_reachable_roots = get_resources_by_name_prefix(
      m_options.assume_reachable_prefixes, m_res_table->name_to_ids);
  // Configured roots by resource type. These should be traversed like any other
  // reachable root.
  auto disallowed_type_ids =
      m_res_table->get_types_by_name(m_options.disallowed_types);
  auto disallowed_resouces = get_disallowed_resources(
      m_res_table->sorted_res_ids, disallowed_type_ids);
  m_assumed_roots = assumed_reachable_roots.size() + disallowed_resouces.size();

  std::unordered_set<uint32_t> result;
  result.insert(manifest_roots.begin(), manifest_roots.end());
  result.insert(ids_from_code.begin(), ids_from_code.end());
  result.insert(assumed_reachable_roots.begin(), assumed_reachable_roots.end());
  result.insert(disallowed_resouces.begin(), disallowed_resouces.end());
  return result;
}

std::unordered_set<uint32_t> ReachableResources::compute_transitive_closure(
    const std::unordered_set<uint32_t>& roots) {
  std::unordered_set<uint32_t> nodes_visited;
  std::unordered_set<std::string> potential_file_paths;
  for (uint32_t root : roots) {
    m_res_table->walk_references_for_resource(
        root, ResourcePathType::ZipPath, &nodes_visited, &potential_file_paths);
  }

  std::unordered_set<std::string> next_xml_files;
  while (!potential_file_paths.empty()) {
    for (auto& str : potential_file_paths) {
      if (is_resource_xml(str)) {
        auto r_str = std::string(m_zip_dir).append("/").append(str);
        if (m_explored_xml_files.find(r_str) == m_explored_xml_files.end()) {
          next_xml_files.emplace(std::move(r_str));
        }
      }
    }

    potential_file_paths.clear();
    for (auto& str : next_xml_files) {
      m_explored_xml_files.emplace(str);
      for (uint32_t attribute :
           m_resources->get_xml_reference_attributes(str)) {
        m_res_table->walk_references_for_resource(
            attribute, ResourcePathType::ZipPath, &nodes_visited,
            &potential_file_paths);
      }
    }
    next_xml_files.clear();
  }
  return nodes_visited;
}
} // namespace resources
