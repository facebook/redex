/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include <string_view>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexStore.h"
#include "GlobalConfig.h"
#include "IRCode.h"
#include "Macros.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "RedexMappedFile.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlockConsistencyCheck.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

using namespace cfg;

namespace {
source_blocks::InsertResult source_blocks(
    DexMethod* method,
    IRCode* code,
    const std::vector<source_blocks::ProfileData>& profiles,
    bool serialize,
    bool exc_inject) {
  ScopedCFG cfg(code);
  return source_blocks::insert_source_blocks(method, cfg.get(), profiles,
                                             serialize, exc_inject);
}

constexpr const char* kAccessName = "access$";

std::optional<std::pair<std::string_view, std::string_view>> is_access_method(
    const std::string_view& full_descriptor) {
  auto tokens = dex_member_refs::parse_method<true>(full_descriptor);
  if (tokens.name.substr(0, 7) != kAccessName) {
    return std::nullopt;
  }
  return std::make_pair(tokens.cls, tokens.name.substr(7));
}

std::optional<std::pair<const DexType*, std::string_view>> is_access_method(
    const DexMethodRef* mref) {
  auto name = mref->get_name()->str();
  if (name.substr(0, 7) != kAccessName) {
    return std::nullopt;
  }
  return std::make_pair(mref->get_class(), name.substr(7));
}

using namespace boost::multi_index;

struct ProfileFile {
  RedexMappedFile mapped_file;
  std::string interaction;

  using StringPos = std::pair<size_t, size_t>;

  using MethodMeta = std::unordered_map<const DexMethodRef*, StringPos>;
  MethodMeta method_meta;

  struct StringViewEquals {
    bool operator()(const std::string& s1, const std::string& s2) const {
      return s1 == s2;
    }
    bool operator()(const std::string& s1, const std::string_view& v2) const {
      return v2 == s1;
    }
    bool operator()(const std::string_view& v1, const std::string& s2) const {
      return v1 == s2;
    }
    bool operator()(const std::string_view& v1,
                    const std::string_view& v2) const {
      return v1 == v2;
    }
  };
  using UnresolvedMethodMeta = multi_index_container<
      std::pair<std::string_view, StringPos>,
      indexed_by<
          hashed_unique<member<std::pair<std::string_view, StringPos>,
                               std::string_view,
                               &std::pair<std::string_view, StringPos>::first>,
                        boost::hash<std::string_view>,
                        StringViewEquals>>>;

  UnresolvedMethodMeta unresolved_method_meta;

  using ClassAccessMethods = multi_index_container<
      std::pair<std::string_view, StringPos>,
      indexed_by<
          hashed_unique<member<std::pair<std::string_view, StringPos>,
                               std::string_view,
                               &std::pair<std::string_view, StringPos>::first>,
                        boost::hash<std::string_view>,
                        StringViewEquals>>>;

  using AccessMethods = std::unordered_map<const DexType*, ClassAccessMethods>;

  AccessMethods access_methods;

  ProfileFile(RedexMappedFile mapped_file,
              std::string interaction,
              MethodMeta method_meta,
              UnresolvedMethodMeta unresolved_method_meta,
              AccessMethods access_methods)
      : mapped_file(std::move(mapped_file)),
        interaction(std::move(interaction)),
        method_meta(std::move(method_meta)),
        unresolved_method_meta(std::move(unresolved_method_meta)),
        access_methods(std::move(access_methods)) {}

  static std::unique_ptr<ProfileFile> prepare_profile_file(
      const std::string& profile_file_name) {
    if (profile_file_name.empty()) {
      return std::unique_ptr<ProfileFile>();
    }
    auto file = RedexMappedFile::open(profile_file_name, /*read_only=*/true);
    MethodMeta meta;
    UnresolvedMethodMeta unresolved_meta;
    AccessMethods access_methods;

    std::string_view data{file.const_data(), file.size()};
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
                          std::string(line).c_str(),
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
      [&data, &src_pos, &linefeed_pos, &meta, &unresolved_meta,
       &access_methods]() {
        size_t comma_pos = data.find(',', src_pos);
        always_assert(comma_pos < linefeed_pos);

        auto string_pos =
            std::make_pair(comma_pos + 1, linefeed_pos - comma_pos - 1);

        auto method_view = data.substr(src_pos, comma_pos - src_pos);

        if (auto access_val = is_access_method(method_view)) {
          auto* access_class = DexType::get_type(access_val->first);
          if (access_class != nullptr) {
            access_methods[access_class].emplace(access_val->second,
                                                 string_pos);
            return;
          }
          TRACE(METH_PROF,
                6,
                "failed to resolve class %s for access method",
                std::string(access_val->first).c_str());
        }

        auto mref = DexMethod::get_method</*kCheckFormat=*/true>(method_view);
        if (mref == nullptr) {
          TRACE(METH_PROF,
                6,
                "failed to resolve %s",
                std::string(method_view).c_str());
          unresolved_meta.emplace(method_view, string_pos);
          return;
        }
        meta.emplace(mref, string_pos);
      }();
    }

    return std::make_unique<ProfileFile>(
        std::move(file), std::move(interaction), std::move(meta),
        std::move(unresolved_meta), std::move(access_methods));
  }
};

struct Injector {
  ConfigFiles& conf;
  std::vector<std::unique_ptr<ProfileFile>> profile_files;
  std::vector<std::string> interactions;
  bool always_inject;

  Injector(ConfigFiles& conf, bool always_inject)
      : conf(conf), always_inject(always_inject) {}

  boost::optional<SourceBlock::Val> maybe_val_from_mp(
      const std::string& interaction, const DexMethodRef* mref) {
    auto& method_profiles = conf.get_method_profiles();
    if (!method_profiles.has_stats()) {
      return boost::none;
    }

    const auto& mp_map = method_profiles.all_interactions();
    const auto& inter_it = mp_map.find(interaction);
    if (inter_it == mp_map.end()) {
      return boost::none;
    }

    const auto& inter_map = inter_it->second;
    auto it = inter_map.find(mref);
    if (it == inter_map.end()) {
      return boost::none;
    }

    // For now, just convert to coverage. Having stats means it's not zero.
    redex_assert(it->second.call_count > 0);
    return SourceBlock::Val(1, it->second.appear_percent);
  }

  using ProfileResult =
      std::pair<std::vector<source_blocks::ProfileData>, bool>;

  ProfileResult empty_profile_files(const DexMethodRef* mref) {
    std::vector<source_blocks::ProfileData> profiles;

    if (always_inject) {
      profiles.reserve(interactions.size());
      auto& method_profiles = conf.get_method_profiles();
      // Some effort to recover from method profiles in general.
      redex_assert(method_profiles.has_stats() || interactions.empty());

      for (const auto& inter : interactions) {
        auto val_opt = maybe_val_from_mp(inter, mref);
        profiles.emplace_back(val_opt ? *val_opt : SourceBlock::Val(0, 0));
      }
    }
    return std::make_pair(std::move(profiles), false);
  }

  ProfileResult find_profiles(const DexMethodRef* mref) {
    auto val_to_str = [](const auto& v) -> std::string {
      if (!v) {
        return "x";
      }
      return std::to_string(v->val) + ":" + std::to_string(v->appear100);
    };
    auto maybe_val_to_str = [&val_to_str](const auto& val_opt) {
      return val_opt ? val_to_str(*val_opt) : "n/a";
    };

    if (profile_files.empty()) {
      return empty_profile_files(mref);
    }

    std::vector<source_blocks::ProfileData> profiles;
    profiles.reserve(profile_files.size());

    auto access_method = is_access_method(mref);

    bool found_one = false;
    for (auto& profile_file : profile_files) {
      auto val_opt = maybe_val_from_mp(profile_file->interaction, mref);

      auto maybe_strpos = [&]() -> std::optional<ProfileFile::StringPos> {
        if (access_method) {
          auto it = profile_file->access_methods.find(access_method->first);
          if (it != profile_file->access_methods.end()) {
            auto it2 = it->second.find(access_method->second);
            if (it2 != it->second.end()) {
              return it2->second;
            }
          }
        }

        auto it = profile_file->method_meta.find(mref);
        if (it != profile_file->method_meta.end()) {
          return it->second;
        }

        return std::nullopt;
      }();

      if (!maybe_strpos) {
        if (always_inject) {
          TRACE(METH_PROF, 3,
                "No basic block profile for %s. Always-inject=true, falling "
                "back to method profiles: %s",
                SHOW(mref),
                [&]() {
                  if (!val_opt) {
                    return "no-profile=" + val_to_str(SourceBlock::Val(0, 0));
                  }
                  return "profile=" + val_to_str(*val_opt);
                }()
                    .c_str());
          profiles.emplace_back(val_opt ? *val_opt : SourceBlock::Val(0, 0));
        } else {
          TRACE(METH_PROF, 3,
                "No basic block profile for %s. Always-inject=false, not "
                "injecting.",
                SHOW(mref));
          profiles.emplace_back(std::nullopt);
        }
        continue;
      }

      found_one = true;
      profiles.emplace_back(
          std::make_pair(std::string(profile_file->mapped_file.const_data() +
                                         maybe_strpos->first,
                                     maybe_strpos->second),
                         val_opt));
      TRACE(METH_PROF, 3,
            "Found basic block profile for %s. Error fallback is %s.",
            SHOW(mref), maybe_val_to_str(val_opt).c_str());
    }

    if (!found_one) {
      TRACE(METH_PROF, 2, "No basic block profile for %s!", SHOW(mref));
    }
    return std::make_pair(std::move(profiles), found_one);
  }

  void run_source_blocks(DexStoresVector& stores,
                         PassManager& mgr,
                         bool serialize,
                         bool exc_inject) {
    auto scope = build_class_scope(stores);

    std::mutex serialized_guard;
    std::vector<std::pair<const DexMethod*, std::string>> serialized;
    size_t blocks{0};
    size_t profile_count{0};

    std::atomic<size_t> skipped{0};

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

    if (conf.get_global_config()
            .get_config_by_name<AssessorConfig>("assessor")
            ->run_sb_consistency) {
      source_blocks::get_sbcc().initialize(scope);
    }

    mgr.set_metric("inserted_source_blocks", blocks);
    mgr.set_metric("handled_methods", serialized.size());
    mgr.set_metric("skipped_methods", skipped.load());
    mgr.set_metric("methods_with_profiles", profile_count);

    {
      size_t unresolved = 0;
      for (const auto& p_file : profile_files) {
        unresolved += p_file->unresolved_method_meta.size();
      }
      mgr.set_metric("avg_unresolved_methods_100",
                     unresolved > 0
                         ? (int64_t)(unresolved * 100.0 / profile_files.size())
                         : 0);
    }

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

  template <typename T, typename Fn>
  void sort_coldstart_and_set_indices(T& container, const Fn& fn) {
    std::sort(container.begin(), container.end(),
              [&fn](const auto& lhs_in, const auto& rhs_in) {
                if (lhs_in == rhs_in) {
                  return false;
                }
                const auto& lhs = fn(lhs_in);
                const auto& rhs = fn(rhs_in);
                if (lhs == rhs) {
                  return false;
                }
                if (lhs == "ColdStart") {
                  return true;
                }
                if (rhs == "ColdStart") {
                  return false;
                }
                return lhs < rhs;
              });

    std::unordered_map<std::string, size_t> interaction_indices;
    for (size_t i = 0; i != container.size(); ++i) {
      interaction_indices[fn(container[i])] = i;
    }
    g_redex->set_sb_interaction_index(interaction_indices);
  }

  void prepare_profile_files_and_interactions(
      const std::string& profile_files_str) {
    if (!profile_files_str.empty()) {
      Timer t("reading files");
      std::vector<std::string> files;
      boost::split(files, profile_files_str, [](const auto& c) {
        constexpr char separator =
#if IS_WINDOWS
            ';';
#else
          ':';
#endif
        return c == separator;
      });

      profile_files.resize(files.size());
      workqueue_run_for<size_t>(0, files.size(), [&](size_t i) {
        profile_files.at(i) = ProfileFile::prepare_profile_file(files.at(i));
        TRACE(METH_PROF, 1, "Loaded basic block profile %s",
              profile_files.at(i)->interaction.c_str());
      });

      // Sort the interactions.
      sort_coldstart_and_set_indices(
          profile_files,
          [](const auto& u) -> const std::string& { return u->interaction; });

      std::transform(profile_files.begin(), profile_files.end(),
                     std::back_inserter(interactions),
                     [](const auto& p) { return p->interaction; });
    } else if (always_inject) {
      // Need to recover interaction names from method profiles.
      if (conf.get_method_profiles().has_stats()) {
        const auto& mp_map = conf.get_method_profiles().all_interactions();
        std::transform(mp_map.begin(), mp_map.end(),
                       std::back_inserter(interactions),
                       [](const auto& p) { return p.first; });
        sort_coldstart_and_set_indices(
            interactions,
            [](const auto& s) -> const std::string& { return s; });
      }
    }
  }
};

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
  bool is_instr_mode = mgr.get_redex_options().instrument_pass_enabled;
  bool always_inject = m_always_inject || m_force_serialize || is_instr_mode;

  Injector inj(conf, always_inject);

  inj.prepare_profile_files_and_interactions(m_profile_files);

  inj.run_source_blocks(stores,
                        mgr,
                        /* serialize= */ m_force_serialize || is_instr_mode,
                        m_insert_after_excs);
}

static InsertSourceBlocksPass s_pass;
