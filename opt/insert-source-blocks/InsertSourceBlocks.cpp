/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/utility/string_view.hpp>
#include <cstdint>
#include <fstream>
#include <mutex>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRCode.h"
#include "Macros.h"
#include "PassManager.h"
#include "RedexMappedFile.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

using namespace cfg;

namespace {
source_blocks::InsertResult source_blocks(
    DexMethod* method,
    IRCode* code,
    const std::vector<boost::optional<std::string>>& profiles,
    bool serialize,
    bool exc_inject) {
  ScopedCFG cfg(code);
  return source_blocks::insert_source_blocks(method, cfg.get(), profiles,
                                             serialize, exc_inject);
}

using namespace boost::multi_index;

struct ProfileFile {
  RedexMappedFile mapped_file;
  std::string interaction;

  using StringPos = std::pair<size_t, size_t>;

  using MethodMeta = std::unordered_map<const DexMethodRef*, StringPos>;
  MethodMeta method_meta;

  // Consider std::string_view in C++17.
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
  using UnresolvedMethodMeta = multi_index_container<
      std::pair<boost::string_view, StringPos>,
      indexed_by<hashed_unique<
          member<std::pair<boost::string_view, StringPos>,
                 boost::string_view,
                 &std::pair<boost::string_view, StringPos>::first>,
          boost::hash<boost::string_view>,
          StringViewEquals>>>;

  UnresolvedMethodMeta unresolved_method_meta;

  ProfileFile(RedexMappedFile mapped_file,
              std::string interaction,
              MethodMeta method_meta,
              UnresolvedMethodMeta unresolved_method_meta)
      : mapped_file(std::move(mapped_file)),
        interaction(std::move(interaction)),
        method_meta(std::move(method_meta)),
        unresolved_method_meta(std::move(unresolved_method_meta)) {}

  static std::unique_ptr<ProfileFile> prepare_profile_file(
      const std::string& profile_file_name) {
    if (profile_file_name.empty()) {
      return std::unique_ptr<ProfileFile>();
    }
    auto file = RedexMappedFile::open(profile_file_name, /*read_only=*/true);
    MethodMeta meta;
    UnresolvedMethodMeta unresolved_meta;

    boost::string_view data{file.const_data(), file.size()};
    size_t pos = 0;
    std::string interaction;

    // Read header.
    {
      auto next_line_fn = [&data, &pos]() {
        always_assert(pos < data.size());
        size_t newline_pos = data.find('\n', pos);
        always_assert(newline_pos != std::string::npos);
        auto ret = data.substr(pos, newline_pos - pos);
        pos = newline_pos + 1;
        return ret;
      };
      auto check_components = [](const auto& line, size_t num = 0,
                                 const std::vector<std::string>& exp = {}) {
        std::vector<std::string> split_vec;
        boost::split(split_vec, line, [](const auto& c) { return c == ','; });
        always_assert_log(num == 0 ? split_vec == exp : split_vec.size() == num,
                          "Unexpected line: %s (%s). Expected %s/%zu.",
                          line.to_string().c_str(),
                          boost::join(split_vec, "'").c_str(),
                          boost::join(exp, ",").c_str(), num);
        return split_vec;
      };
      check_components(next_line_fn(), 0, {"interaction", "appear#"});
      {
        auto line = next_line_fn();
        std::vector<std::string> split_vec;
        boost::split(split_vec, line, [](const auto& c) { return c == ','; });
        always_assert(split_vec.size() == 2);
        interaction = std::move(split_vec[0]);
      }
      check_components(next_line_fn(), 0, {"name", "profiled_srcblks_exprs"});
    }

    while (pos < data.length()) {
      const size_t src_pos = pos;

      // Find the next '\n' or EOF.
      size_t linefeed_pos = data.find('\n', src_pos);
      if (linefeed_pos == std::string::npos) {
        linefeed_pos = data.length();
      }
      pos = linefeed_pos + 1;
      // Do not use pos anymore! Ensure by scope from lambda.
      [&data, &src_pos, &linefeed_pos, &meta, &unresolved_meta]() {
        size_t comma_pos = data.find(',', src_pos);
        always_assert(comma_pos < linefeed_pos);

        auto string_pos =
            std::make_pair(comma_pos + 1, linefeed_pos - comma_pos - 1);

        auto method_view = data.substr(src_pos, comma_pos - src_pos);
        // Would be nice to avoid the conversion.
        auto mref = DexMethod::get_method</*kCheckFormat=*/true>(
            method_view.to_string());
        if (mref == nullptr) {
          TRACE(METH_PROF,
                6,
                "failed to resolve %s",
                method_view.to_string().c_str());
          unresolved_meta.emplace(method_view, string_pos);
          return;
        }
        meta.emplace(mref, string_pos);
      }();
    }

    return std::make_unique<ProfileFile>(
        std::move(file), std::move(interaction), std::move(meta),
        std::move(unresolved_meta));
  }
};

void run_source_blocks(
    DexStoresVector& stores,
    ConfigFiles& conf,
    PassManager& mgr,
    bool serialize,
    bool exc_inject,
    bool always_inject,
    std::vector<std::unique_ptr<ProfileFile>>& profile_files) {
  auto scope = build_class_scope(stores);

  std::mutex serialized_guard;
  std::vector<std::pair<const DexMethod*, std::string>> serialized;
  size_t blocks{0};
  size_t profile_count{0};

  std::atomic<size_t> skipped{0};

  auto find_profiles = [&profile_files](const DexMethodRef* mref) {
    if (profile_files.empty()) {
      return std::make_pair(std::vector<boost::optional<std::string>>(), false);
    }
    std::vector<boost::optional<std::string>> profiles;
    bool found_one = false;
    for (auto& profile_file : profile_files) {
      auto it = profile_file->method_meta.find(mref);
      if (it == profile_file->method_meta.end()) {
        profiles.emplace_back(boost::none);
        continue;
      }
      found_one = true;
      const auto& pos = it->second;
      profiles.emplace_back(std::string(
          profile_file->mapped_file.const_data() + pos.first, pos.second));
    }
    if (!found_one) {
      TRACE(METH_PROF, 2, "No basic block profile for %s!", SHOW(mref));
    }
    return std::make_pair(std::move(profiles), found_one);
  };
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code != nullptr) {
      auto profiles = find_profiles(method);
      if (!profiles.second && !always_inject) {
        // Skip without profile.
        skipped.fetch_add(1);
        return;
      }
      auto res =
          source_blocks(method, code, profiles.first, serialize, exc_inject);
      std::unique_lock<std::mutex> lock(serialized_guard);
      serialized.emplace_back(method, std::move(res.serialized));
      blocks += res.block_count;
      profile_count += profiles.second ? 1 : 0;
    }
  });
  mgr.set_metric("inserted_source_blocks", blocks);
  mgr.set_metric("handled_methods", serialized.size());
  mgr.set_metric("skipped_methods", skipped.load());
  mgr.set_metric("methods_with_profiles", profile_count);

  if (!serialize) {
    return;
  }

  std::sort(serialized.begin(),
            serialized.end(),
            [](const auto& lhs, const auto& rhs) {
              return compare_dexmethods(lhs.first, rhs.first);
            });

  std::ofstream ofs(conf.metafile("redex-source-blocks.csv"));
  ofs << "type,version\nredex-source-blocks,1\nname,serialized\n";
  for (const auto& p : serialized) {
    ofs << show(p.first) << "," << p.second << "\n";
  }
}
} // namespace

void InsertSourceBlocksPass::bind_config() {
  bind("always_inject",
       m_always_inject,
       m_always_inject,
       "Always inject source blocks, even if profiles are missing.");
  bind("force_run", m_force_run, m_force_run);
  bind("force_serialize",
       m_force_serialize,
       m_force_serialize,
       "Force serialization of the CFGs. Testing only.");
  bind("insert_after_excs", m_insert_after_excs, m_insert_after_excs);
  bind("profile_files", "", m_profile_files);
}

void InsertSourceBlocksPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  // TODO(agampe): This should eventually go away. For now, avoid the
  // overhead.
  if (!mgr.get_redex_options().instrument_pass_enabled && !m_force_run) {
    TRACE(METH_PROF,
          1,
          "Not an instrumentation build, not running InsertSourceBlocksPass");
    return;
  }

  std::vector<std::unique_ptr<ProfileFile>> profile_files;
  if (!m_profile_files.empty()) {
    std::vector<std::string> files;
    boost::split(files, m_profile_files, [](const auto& c) {
      constexpr char separator =
#if IS_WINDOWS
          ';';
#else
          ':';
#endif
      return c == separator;
    });
    for (const auto& file : files) {
      profile_files.emplace_back(ProfileFile::prepare_profile_file(file));
      TRACE(METH_PROF, 1, "Loaded basic block profile %s",
            profile_files.back()->interaction.c_str());
    }
    // Sort the interactions.
    std::sort(profile_files.begin(), profile_files.end(),
              [](const auto& lhs, const auto& rhs) {
                if (lhs == rhs) {
                  return false;
                }
                if (lhs->interaction == "ColdStart") {
                  return true;
                }
                if (rhs->interaction == "ColdStart") {
                  return false;
                }
                return lhs->interaction < rhs->interaction;
              });
    std::unordered_map<std::string, size_t> indices;
    for (size_t i = 0; i != profile_files.size(); ++i) {
      indices[profile_files[i]->interaction] = i;
    }
    g_redex->set_sb_interaction_index(indices);
  }

  bool is_instr_mode = mgr.get_redex_options().instrument_pass_enabled;
  run_source_blocks(stores,
                    conf,
                    mgr,
                    /* serialize= */ m_force_serialize || is_instr_mode,
                    m_insert_after_excs,
                    /* always_inject= */ m_always_inject || m_force_serialize ||
                        is_instr_mode,
                    profile_files);
}

static InsertSourceBlocksPass s_pass;
