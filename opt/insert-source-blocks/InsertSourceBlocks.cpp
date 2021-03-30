/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

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
#include "PassManager.h"
#include "RedexMappedFile.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

using namespace cfg;

namespace {
source_blocks::InsertResult source_blocks(DexMethod* method,
                                          IRCode* code,
                                          const std::string* profile,
                                          bool serialize,
                                          bool exc_inject) {
  ScopedCFG cfg(code);
  return source_blocks::insert_source_blocks(
      method, cfg.get(), profile, serialize, exc_inject);
}

using namespace boost::multi_index;

struct ProfileFile {
  RedexMappedFile mapped_file;

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
              MethodMeta method_meta,
              UnresolvedMethodMeta unresolved_method_meta)
      : mapped_file(std::move(mapped_file)),
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
        std::move(file), std::move(meta), std::move(unresolved_meta));
  }
};

void run_source_blocks(DexStoresVector& stores,
                       ConfigFiles& conf,
                       PassManager& mgr,
                       bool serialize,
                       bool exc_inject,
                       const ProfileFile* profile_file) {
  auto scope = build_class_scope(stores);

  std::mutex serialized_guard;
  std::vector<std::pair<const DexMethod*, std::string>> serialized;
  size_t blocks{0};
  auto find_profile = [profile_file](const DexMethodRef* mref,
                                     std::string& storage) -> std::string* {
    if (profile_file == nullptr) {
      return nullptr;
    }
    auto it = profile_file->method_meta.find(mref);
    if (it == profile_file->method_meta.end()) {
      TRACE(METH_PROF, 1, "No basic block profile for %s!", SHOW(mref));
      return nullptr;
    }
    const auto& pos = it->second;
    storage = std::string(profile_file->mapped_file.const_data() + pos.first,
                          pos.second);
    return &storage;
  };
  walk::parallel::methods(scope, [&](DexMethod* method) {
    auto code = method->get_code();
    if (code != nullptr) {
      std::string profile_storage;
      auto* profile = find_profile(method, profile_storage);
      auto res = source_blocks(method, code, profile, serialize, exc_inject);
      std::unique_lock<std::mutex> lock(serialized_guard);
      serialized.emplace_back(method, std::move(res.serialized));
      blocks += res.block_count;
    }
  });
  mgr.set_metric("inserted_source_blocks", blocks);
  mgr.set_metric("handled_methods", serialized.size());

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
  bind("force_serialize",
       m_force_serialize,
       m_force_serialize,
       "Force serialization of the CFGs. Testing only.");
  bind("insert_after_excs", m_insert_after_excs, m_insert_after_excs);
  bind("profile_file", "", m_profile_file);
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

  std::unique_ptr<ProfileFile> profile_file =
      ProfileFile::prepare_profile_file(m_profile_file);

  run_source_blocks(stores,
                    conf,
                    mgr,
                    mgr.get_redex_options().instrument_pass_enabled ||
                        m_force_serialize,
                    m_insert_after_excs,
                    profile_file.get());
}

static InsertSourceBlocksPass s_pass;
