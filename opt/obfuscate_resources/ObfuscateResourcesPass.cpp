/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObfuscateResourcesPass.h"

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "ApkResources.h"
#include "BundleResources.h"
#include "ConfigFiles.h"
#include "Debug.h"
#include "DetectBundle.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IOUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "RedexResources.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

const std::string RESID_TO_NAME_FILENAME = "resid_to_name.json";
const std::string RESFILE_MAPPING = "resource-mapping.txt";
const std::string DOT_DELIM = ".";
const std::string RES_START = std::string(RES_DIRECTORY) + "/";
const std::string SHORTEN_START = std::string(OBFUSCATED_RES_DIRECTORY) + "/";
const std::string PORT_CHAR = "abcdefghijklmnopqrstuvwxyz0123456789_-";
const std::string FONT_DIR = "/font/";

std::string get_short_name_from_index(size_t index) {
  always_assert(index >= 0);
  std::string to_return;
  auto PORT_CHAR_length = PORT_CHAR.length();
  while (index >= PORT_CHAR_length) {
    size_t i = index % PORT_CHAR_length;
    to_return = PORT_CHAR[i] + to_return;
    index = index / PORT_CHAR_length;
  }
  to_return = PORT_CHAR[index] + to_return;
  return to_return;
}

// TODO(T126661220): move away from detecting resource type from file path
bool is_font_resource(const std::string& filename) {
  return filename.find(FONT_DIR) != std::string::npos;
}

std::string get_short_name(const std::string& filename, size_t index) {
  std::string return_filename;
  std::string file_extension;
  auto find_dot = filename.find(DOT_DELIM);
  if (find_dot != std::string::npos) {
    file_extension += filename.substr(find_dot);
  }
  // For bundle, file don't start with "res/"", but start with "module_name/",
  // we should keep the module_name folder
  auto find_res = filename.find(RES_START);
  always_assert_log(find_res != std::string::npos,
                    "Didn't find 'res/' in filename %s", filename.c_str());
  std::string module_name = filename.substr(0, find_res);

  // Keeping res/ is necessary to make custom font work
  // https://cs.android.com/android/platform/superproject/+/android-9.0.0_r1:frameworks/base/core/java/android/content/res/ResourcesImpl.java;l=898
  if (find_res == 0 && !is_font_resource(filename)) {
    // Apk format, able to rename to r/
    return_filename = module_name + SHORTEN_START +
                      get_short_name_from_index(index) + file_extension;
  } else {
    // Bundle format, need to keep res/, otherwise will crash in bundletool
    // https://github.com/google/bundletool/blob/06296d8ec009af6ec7d09f6da2cf54994fa3a89b/src/main/java/com/android/tools/build/bundletool/validation/BundleFilesValidator.java#L155
    return_filename = module_name + RES_START +
                      get_short_name_from_index(index) + file_extension;
  }

  return return_filename;
}

std::string remove_module(const std::string& filename) {
  auto find_res = filename.find(RES_START);
  if (find_res != std::string::npos) {
    return filename.substr(find_res);
  }
  return filename;
}

void rename_files(const std::string& zip_dir,
                  std::map<std::string, std::string>* filename_old_to_new) {
  std::unordered_set<std::string> not_exists;
  std::unordered_set<std::string> created_file_directory;
  for (const auto& pair : *filename_old_to_new) {
    std::string full_path = zip_dir + "/" + pair.first;
    if (exists(boost::filesystem::path(full_path))) {
      std::string full_path_after = zip_dir + "/" + pair.second;
      auto parent_folder_new =
          boost::filesystem::path(full_path_after).parent_path();
      auto parent_folder_new_str = parent_folder_new.string();
      if (!created_file_directory.count(parent_folder_new_str)) {
        created_file_directory.emplace(parent_folder_new_str);
        boost::filesystem::create_directory(parent_folder_new);
      }
      TRACE(OBFUS_RES, 5, "renaming %s -> %s", full_path.c_str(),
            full_path_after.c_str());
      auto rename_res = std::rename(full_path.c_str(), full_path_after.c_str());
      always_assert(rename_res == 0);
    } else {
      not_exists.emplace(pair.first);
    }
  }
  for (const auto& not_exist : not_exists) {
    filename_old_to_new->erase(not_exist);
  }
}

// Handle patterns like
// https://developer.android.com/reference/androidx/constraintlayout/widget/Barrier#example
void handle_known_resource_name_patterns(
    const std::unordered_set<std::string>& values,
    std::unordered_set<std::string>* possible_resource_names) {
  // check for comma separated list of resource names.
  for (const auto& s : values) {
    if (s.find(',') != std::string::npos) {
      std::vector<std::string> parts;
      boost::split(parts, s, boost::is_any_of(","));
      for (auto& part : parts) {
        boost::trim(part);
        possible_resource_names->emplace(part);
      }
    } else {
      possible_resource_names->emplace(s);
    }
  }
}

// Returns false if the deobfuscated name of item starts with anything in the
// given set.
template <typename Item>
bool should_check_for_strings(
    const std::unordered_set<std::string>& code_to_skip, Item* item) {
  if (code_to_skip.empty()) {
    return true;
  }
  auto item_name = show_deobfuscated(item);
  return std::find_if(code_to_skip.begin(),
                      code_to_skip.end(),
                      [&](const std::string& prefix) {
                        return boost::algorithm::starts_with(item_name, prefix);
                      }) == code_to_skip.end();
}

// Check primarily const-string opcodes and static field values for strings that
// might be resource names. Not meant to be exhaustive (does not check all
// annotations, for example).
void collect_string_values_from_code(
    Scope& scope,
    const std::unordered_set<std::string>& code_to_skip,
    std::unordered_set<std::string>* out) {
  ConcurrentSet<std::string> const_string_values;
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (!should_check_for_strings(code_to_skip, cls)) {
      return;
    }
    std::vector<const DexString*> strings;
    for (const auto& f : cls->get_sfields()) {
      f->gather_strings(strings);
    }
    for (const auto& m : cls->get_all_methods()) {
      if (should_check_for_strings(code_to_skip, m)) {
        auto code = m->get_code();
        // Checking things like proto / type names is probably unnecessary. Just
        // look at instructions.
        if (code != nullptr) {
          code->gather_strings(strings);
        }
      }
    }
    for (const auto& dex_string : strings) {
      auto s = dex_string->str_copy();
      const_string_values.emplace(s);
    }
  });
  out->insert(const_string_values.begin(), const_string_values.end());
}
} // namespace

void ObfuscateResourcesPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  std::string zip_dir;
  conf.get_json_config().get("apk_dir", "", zip_dir);
  if (zip_dir.empty()) {
    return;
  }
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();
  Json::Value resid_to_name_json;
  boost::format hex_format("0x%08x");
  for (const auto& pair : res_table->id_to_name) {
    resid_to_name_json[(hex_format % pair.first).str()] = pair.second;
  }
  write_string_to_file(conf.metafile(RESID_TO_NAME_FILENAME),
                       resid_to_name_json.toStyledString());

  if (!m_obfuscate_resource_name && !m_obfuscate_id_name &&
      !m_obfuscate_resource_file && !m_obfuscate_xml_attributes) {
    TRACE(OBFUS_RES, 1, "Resource obfuscation not enabled.");
    return;
  }

  if (m_obfuscate_xml_attributes) {
    resources->obfuscate_xml_files(m_xml_obfuscation_allowed_types,
                                   m_do_not_obfuscate_elements);
  }

  std::unordered_set<uint32_t> shifted_allow_type_ids;
  if (m_obfuscate_resource_name || m_obfuscate_id_name) {
    if (m_obfuscate_id_name) {
      if (!m_obfuscate_resource_name) {
        m_name_obfuscation_allowed_types.clear();
      }
      m_name_obfuscation_allowed_types.emplace("id");
    }
    std::unordered_set<uint32_t> allow_type_ids =
        res_table->get_types_by_name_prefixes(m_name_obfuscation_allowed_types);
    for (auto& type_id : allow_type_ids) {
      shifted_allow_type_ids.emplace(type_id >> TYPE_INDEX_BIT_SHIFT);
    }
  }

  std::unordered_set<std::string> keep_resource_names_specific;
  if (m_keep_resource_names_from_string_literals) {
    // Rather broad step to search for string constants, in case they could be
    // used as resource identifier lookups. NOTE: This step should happen before
    // file path obfuscation, as traversing directory structure becomes wonky
    // after that point.
    Timer t("resource_names_from_string_literals");
    std::unordered_set<std::string> xml_attribute_values;
    resources->collect_xml_attribute_string_values(&xml_attribute_values);
    handle_known_resource_name_patterns(xml_attribute_values,
                                        &keep_resource_names_specific);
    auto scope = build_class_scope(stores);
    collect_string_values_from_code(scope, m_code_references_okay_to_obfuscate,
                                    &keep_resource_names_specific);
  }

  std::map<std::string, std::string> filepath_old_to_new;
  if (m_obfuscate_resource_file) {
    const auto& sorted_res_ids = res_table->sorted_res_ids;
    std::set<std::string> all_files;
    for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
      auto res_id = sorted_res_ids[index];
      for (const auto& file :
           res_table->get_files_by_rid(res_id, ResourcePathType::ZipPath)) {
        all_files.emplace(file);
      }
    }
    size_t index = 0;
    for (const auto& filename : all_files) {
      if (std::find_if(m_keep_resource_file_names.begin(),
                       m_keep_resource_file_names.end(),
                       [&](const std::string& v) {
                         return filename.find(v) != std::string::npos;
                       }) != m_keep_resource_file_names.end()) {
        TRACE(OBFUS_RES, 5, "Not obfuscating %s within keep list",
              filename.c_str());
      } else {
        filepath_old_to_new[filename] = get_short_name(filename, index);
        TRACE(OBFUS_RES, 5, "%s -> %s", filename.c_str(),
              filepath_old_to_new[filename].c_str());
        ++index;
      }
    }
    rename_files(zip_dir, &filepath_old_to_new);
    std::unordered_set<std::string> existing_old_name;
    std::unordered_set<std::string> existing_new_name;
    if (!filepath_old_to_new.empty()) {
      Json::Value resfile_mapping_json;
      for (const auto& pair : filepath_old_to_new) {
        const auto& old_name = remove_module(pair.first);
        const auto& new_name = remove_module(pair.second);
        // Sanity check
        always_assert(
            existing_old_name.find(old_name) == existing_old_name.end() &&
            existing_new_name.find(new_name) == existing_new_name.end());
        existing_old_name.emplace(old_name);
        existing_new_name.emplace(new_name);
        resfile_mapping_json[old_name] = new_name;
      }
      write_string_to_file(conf.metafile(RESFILE_MAPPING),
                           resfile_mapping_json.toStyledString());
    }
  }

  const auto& res_files = resources->find_resources_files();
  auto changed = res_table->obfuscate_resource_and_serialize(
      res_files, filepath_old_to_new, shifted_allow_type_ids,
      m_keep_resource_name_prefixes, keep_resource_names_specific);
  mgr.incr_metric("num_anonymized_resource_names", changed);
  mgr.incr_metric("num_anonymized_resource_files", filepath_old_to_new.size());
}

static ObfuscateResourcesPass s_pass;
