/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>

#include <boost/format.hpp>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexStore.h"
#include "GlobalConfig.h"
#include "IRCode.h"
#include "Macros.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "RedexContext.h"
#include "RedexMappedFile.h"
#include "Show.h"
#include "SourceBlockConsistencyCheck.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

using namespace cfg;

namespace {

// Access methods do not have a stable naming scheme in Javac. It seems a
// running counter is used with the first reference to a member.
//
// At the same time, Kotlin seems to generate complex-named access methods
// that include the accessed member in the name. We should not touch this.
//
// To deal with this, we will hash Java's access method's contents in the hope
// that it is simple and stable. We prefix the hash name with "redex" in the
// hope to detect it properly. (We could also use a purely decimal
// representation, but hex is simpler and more standard.)

bool is_numeric(const std::string_view& s) {
  return std::all_of(s.begin(), s.end(),
                     [](auto c) { return '0' <= c && c <= '9'; });
}

namespace hasher {

uint64_t stable_hash_value(const std::string& s) {
  uint64_t stable_hash{s.size()};
  for (auto c : s) {
    stable_hash = stable_hash * 3 + c;
  }
  return stable_hash;
}
uint64_t stable_hash_value(const IRInstruction* insn) {
  uint64_t stable_hash = static_cast<uint64_t>(insn->opcode());
  switch (opcode::ref(insn->opcode())) {
  case opcode::Ref::Method:
    stable_hash =
        stable_hash * 41 + stable_hash_value(show(insn->get_method()));
    break;
  case opcode::Ref::Field:
    stable_hash = stable_hash * 43 + stable_hash_value(show(insn->get_field()));
    break;
  case opcode::Ref::String:
    stable_hash =
        stable_hash * 47 + stable_hash_value(show(insn->get_string()));
    break;
  case opcode::Ref::Type:
    stable_hash = stable_hash * 53 + stable_hash_value(show(insn->get_type()));
    break;
  case opcode::Ref::Data:
    stable_hash = stable_hash * 59 + insn->get_data()->size();
    break;
  case opcode::Ref::Literal:
    stable_hash = stable_hash * 61 + insn->get_literal();
    break;
  case opcode::Ref::MethodHandle:
  case opcode::Ref::CallSite:
  case opcode::Ref::Proto:
    always_assert_log(false, "Unsupported Ref");
    __builtin_unreachable();
  case opcode::Ref::None:
    break;
  }

  for (auto reg : insn->srcs()) {
    stable_hash = stable_hash * 3 + reg;
  }
  if (insn->has_dest()) {
    stable_hash = stable_hash * 5 + insn->dest();
  }

  return stable_hash;
}

uint64_t stable_hash(ControlFlowGraph& cfg) {
  // We need a stable iteration order, no matter how blocks were constructed.
  // The actual order does not matter, so do a BFS because that doesn't have
  // recursive depth problems.

  uint64_t hash = 0;

  std::deque<Block*> queue;
  UnorderedSet<Block*> seen;

  auto push = [&queue, &seen](auto* b) {
    if (seen.insert(b).second) {
      queue.push_back(b);
    }
  };

  push(cfg.entry_block());
  while (!queue.empty()) {
    auto* cur = queue.front();
    queue.pop_front();

    hash = hash * 3 + 1;

    for (auto& mie : *cur) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }
      auto insn_hash = stable_hash_value(mie.insn);
      hash = hash * 5 + insn_hash;
    }

    // Handle outgoing edges.
    auto succs = source_blocks::impl::get_sorted_edges(cur);
    for (auto* e : succs) {
      hash = hash * 7 + static_cast<uint64_t>(e->type());
      hash = hash * 3 + [e]() {
        switch (e->type()) {
        case EDGE_GOTO:
          return 0;
        case EDGE_BRANCH:
          return 1;
        case EDGE_THROW:
          return 2;
        case EDGE_GHOST:
        case EDGE_TYPE_SIZE:
          not_reached();
          return 0;
        }
      }();
      hash = hash * 23 + [e]() -> uint64_t {
        switch (e->type()) {
        case EDGE_GOTO:
          return 0;
        case EDGE_BRANCH:
          return e->case_key() ? *e->case_key() : 1;
        case EDGE_THROW: {
          auto* t = e->throw_info();
          return (t->catch_type == nullptr
                      ? 0
                      : stable_hash_value(show(t->catch_type))) *
                     5 +
                 t->index;
        }
        case EDGE_GHOST:
        case EDGE_TYPE_SIZE:
          not_reached();
          return 0;
        }
      }();
    }
  }

  return hash;
}

// Try to use a name that is unlikely to be used by someone in code and then
// Kotlin generates it.
std::string hashed_name(uint64_t hash_value,
                        const std::string_view& access_method_name) {
  // The modern javac way encodes access flags in the last two digits of the
  // numerical suffix. Unfortunately we may also see older implementations (or
  // maybe written by hand or bytecode frameworks).
  // In that case, hope that it is single- or double-digit. Do not cross-check
  // a class, that adds complexity and is not worth it (just detect at most
  // 0-99). Then just use `00` for flags, relying solely on the body hash.

  return std::string("redex") + (boost::format("%016x") % hash_value).str() +
         "$" +
         (access_method_name.size() >= 3
              ? std::string(access_method_name.substr(
                    access_method_name.length() - 2, 2))
              : std::string("00"));
}

bool maybe_hashed_name(const std::string_view& access_part) {
  if (access_part.length() !=
      5 /* redex */ + 16 + /* hash */ +1 /* $ */ + 2 /* flags */) {
    return false;
  }
  if (access_part.substr(0, 5) != "redex") {
    return false;
  }
  {
    auto hash_part = access_part.substr(5, 16);
    if (!std::all_of(hash_part.begin(), hash_part.end(), [](auto c) {
          return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f');
        })) {
      return false;
    }
  }
  if (access_part[5 + 16] != '$') {
    return false;
  }
  return is_numeric(access_part.substr(5 + 16 + 1, 2));
}

} // namespace hasher

// NOTE: It looks like the Kotlin compiler does not follow the Javac naming
//       scheme, using names instead. Let's rely on those names being stable.

constexpr const char* kAccessName = "access$";

std::optional<std::pair<std::string_view, std::string_view>>
is_traditional_access_method(const std::string_view& full_descriptor) {
  auto tokens = dex_member_refs::parse_method<true>(full_descriptor);
  if (tokens.name.substr(0, 7) != kAccessName) {
    return std::nullopt;
  }
  auto access_name = tokens.name.substr(7);
  if (!is_numeric(access_name) && !hasher::maybe_hashed_name(access_name)) {
    return std::nullopt;
  }
  return std::make_pair(tokens.cls, access_name);
}

std::optional<std::pair<const DexType*, std::string_view>>
is_traditional_access_method(const DexMethodRef* mref) {
  auto name = mref->get_name()->str();
  if (name.substr(0, 7) != kAccessName) {
    return std::nullopt;
  }
  auto access_name = name.substr(7);
  // Note: we do not rename the methods, so this should be a Java-style number.
  if (!is_numeric(access_name)) {
    return std::nullopt;
  }
  return std::make_pair(mref->get_class(), access_name);
}

struct ProfileFile {
  RedexMappedFile mapped_file;
  std::string interaction;

  using StringPos = std::pair<size_t, size_t>;

  using MethodMeta = UnorderedMap<const DexMethodRef*, StringPos>;
  MethodMeta method_meta;

  using UnresolvedMethods = UnorderedSet<std::string_view>;

  UnresolvedMethods unresolved_methods;

  using ClassAccessMethods = UnorderedMap<std::string_view, StringPos>;

  using AccessMethods = UnorderedMap<const DexType*, ClassAccessMethods>;

  AccessMethods access_methods;

  ProfileFile(RedexMappedFile mapped_file,
              std::string interaction,
              MethodMeta method_meta,
              UnresolvedMethods unresolved_methods,
              AccessMethods access_methods)
      : mapped_file(std::move(mapped_file)),
        interaction(std::move(interaction)),
        method_meta(std::move(method_meta)),
        unresolved_methods(std::move(unresolved_methods)),
        access_methods(std::move(access_methods)) {}

  static std::unique_ptr<ProfileFile> prepare_profile_file(
      const std::string& profile_file_name) {
    if (profile_file_name.empty()) {
      return std::unique_ptr<ProfileFile>();
    }
    auto file = RedexMappedFile::open(profile_file_name, /*read_only=*/true);
    MethodMeta meta;
    UnresolvedMethods unresolved_methods;
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
      [&data, &src_pos, &linefeed_pos, &meta, &unresolved_methods,
       &access_methods]() {
        size_t comma_pos = data.find(',', src_pos);
        always_assert(comma_pos < linefeed_pos);

        auto string_pos =
            std::make_pair(comma_pos + 1, linefeed_pos - comma_pos - 1);

        auto method_view = data.substr(src_pos, comma_pos - src_pos);

        if (auto access_val = is_traditional_access_method(method_view)) {
          auto* access_class = DexType::get_type(access_val->first);
          if (access_class != nullptr) {
            TRACE(METH_PROF, 7, "Found access method %s",
                  std::string(method_view).c_str());
            access_methods[access_class].emplace(access_val->second,
                                                 string_pos);
            return;
          }
          TRACE(METH_PROF,
                6,
                "failed to resolve class %s for access method",
                std::string(access_val->first).c_str());
        }

        auto* mref = DexMethod::get_method</*kCheckFormat=*/true>(method_view);
        if (mref == nullptr) {
          TRACE(METH_PROF,
                6,
                "failed to resolve %s",
                std::string(method_view).c_str());
          unresolved_methods.insert(method_view);
          return;
        }
        TRACE(METH_PROF, 7, "Found normal method %s.",
              std::string(method_view).c_str());
        meta.emplace(mref, string_pos);
      }();
    }

    return std::make_unique<ProfileFile>(
        std::move(file), std::move(interaction), std::move(meta),
        std::move(unresolved_methods), std::move(access_methods));
  }
};

struct Injector {
  ConfigFiles& conf;
  std::vector<std::unique_ptr<ProfileFile>> profile_files;
  std::vector<std::string> interactions;
  bool use_default_value;
  bool use_fuzzing_values;
  bool always_inject;
  bool fix_violations;

  Injector(ConfigFiles& conf,
           bool always_inject,
           bool use_default_value,
           bool use_fuzzing_values,
           bool fix_violations)
      : conf(conf),
        use_default_value(use_default_value),
        use_fuzzing_values(use_fuzzing_values),
        always_inject(always_inject),
        fix_violations(fix_violations) {
    // Prefetch the method profiles. We may need them when block profiles
    // are missing and it's easier to do it here than have to synchronize
    // loading later. (It's probably also amortized with later passes.)
    conf.get_method_profiles();
  }

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

  ProfileResult find_profiles(DexMethod* mref,
                              const DexType* access_method_type_or_null,
                              std::string_view exact_name,
                              const std::string& hashed_name) {
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

    bool found_one = false;
    for (auto& profile_file : profile_files) {
      auto val_opt = maybe_val_from_mp(profile_file->interaction, mref);

      auto maybe_strpos = [&]() -> std::optional<ProfileFile::StringPos> {
        if (access_method_type_or_null != nullptr) {
          auto it =
              profile_file->access_methods.find(access_method_type_or_null);
          if (it != profile_file->access_methods.end()) {
            auto& map = it->second;
            // Try hashed name first, new style.
            auto it2 = map.find(hashed_name);
            if (it2 != map.end()) {
              TRACE(METH_PROF, 7, "Found hashed access method %s for %s",
                    hashed_name.c_str(), SHOW(mref));
              return it2->second;
            }

            // Try original name, legacy/transition.
            it2 = map.find(exact_name);
            if (it2 != map.end()) {
              TRACE(METH_PROF, 7, "Found exact access method %s for %s",
                    std::string(exact_name).c_str(), SHOW(mref));
              return it2->second;
            }

            TRACE(METH_PROF, 3,
                  "Did not find an access method for %s/%s in %s\n%s",
                  std::string(exact_name).c_str(), hashed_name.c_str(),
                  SHOW(access_method_type_or_null),
                  [&]() {
                    std::string res;
                    for (auto& p : UnorderedIterable(map)) {
                      res.append(p.first);
                      res.append(", ");
                    }
                    return res;
                  }()
                      .c_str());
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

  struct MethodFuzzingMetadata {
    size_t indegrees{0};
    size_t insertion_id{0};
    bool has_values{false};
    int hit;

    MethodFuzzingMetadata(size_t indegrees, size_t insertion_id)
        : indegrees(indegrees), insertion_id(insertion_id) {}

    bool operator<(const MethodFuzzingMetadata& r) const {
      if (indegrees == r.indegrees) {
        return insertion_id < r.insertion_id;
      }
      return indegrees < r.indegrees;
    }
  };

  template <typename NodeIdFn>
  void topo_traverse_callgraph(
      UnorderedMap<call_graph::NodeId, MethodFuzzingMetadata>& metadata,
      call_graph::Graph& call_graph,
      const NodeIdFn& nodeid_fn) {

    auto topo_comparator = [&](call_graph::NodeId l, call_graph::NodeId r) {
      return metadata.at(l) < metadata.at(r);
    };

    std::multiset<call_graph::NodeId, decltype(topo_comparator)> process_queue(
        topo_comparator);
    UnorderedSet<call_graph::NodeId> visited;
    size_t insertion_order_id = 0;
    const auto* start_node = call_graph.entry();

    visited.insert(start_node);
    process_queue.insert(start_node);

    while (!process_queue.empty()) {
      auto temp = process_queue.begin();
      auto current = *temp;
      process_queue.erase(process_queue.begin());

      nodeid_fn(current);

      for (const auto& edge : current->callees()) {
        auto* neighbor = edge->callee();
        bool re_add_neighbor = false;

        if (process_queue.count(neighbor)) {
          process_queue.erase(neighbor);
          re_add_neighbor = true;
        }
        metadata.at(neighbor).indegrees--;
        if (re_add_neighbor) {
          process_queue.insert(neighbor);
        }

        if (!visited.count(neighbor)) {
          metadata.at(neighbor).insertion_id = insertion_order_id;
          ++insertion_order_id;
          visited.insert(neighbor);
          process_queue.insert(neighbor);
        }
      }
    }
  }

  // operator+= does not work well, too much copying around.
  struct SerializedMethodInfo {
    const DexString* method;
    std::string s_expression;
    std::string idom_map;
  };
  struct SimpleSMIStore {
    std::mutex acc_mutex{};
    std::deque<SerializedMethodInfo> data{};

    void add(SerializedMethodInfo&& in) {
      std::unique_lock<std::mutex> lock{acc_mutex};
      data.emplace_back(in);
    }
  };

  struct InsertResult {
    size_t skipped{0};
    size_t blocks{0};
    size_t profile_count{0};
    size_t profile_failed{0};
    size_t access_methods{0};
    size_t hot_src_block_count{0};
    size_t cold_src_block_count{0};
    size_t hot_throw_cold_block_count{0};
    size_t normalized_blocks{0};
    size_t denormalized_blocks{0};
    size_t elided_vals{0};
    size_t unelided_vals{0};

    InsertResult() = default;
    InsertResult(size_t skipped, size_t access_methods)
        : skipped(skipped), access_methods(access_methods) {}
    InsertResult(size_t access_methods,
                 size_t blocks,
                 size_t profile_count,
                 size_t profile_failed,
                 size_t hot_src_block_count,
                 size_t cold_src_block_count,
                 size_t hot_throw_cold_block_count,
                 size_t normalized_blocks,
                 size_t denormalized_blocks,
                 size_t elided_vals,
                 size_t unelided_vals)
        : blocks(blocks),
          profile_count(profile_count),
          profile_failed(profile_failed),
          access_methods(access_methods),
          hot_src_block_count(hot_src_block_count),
          cold_src_block_count(cold_src_block_count),
          hot_throw_cold_block_count(hot_throw_cold_block_count),
          normalized_blocks(normalized_blocks),
          denormalized_blocks(denormalized_blocks),
          elided_vals(elided_vals),
          unelided_vals(unelided_vals) {}

    InsertResult& operator+=(const InsertResult& other) {
      skipped += other.skipped;
      blocks += other.blocks;
      profile_count += other.profile_count;
      profile_failed += other.profile_failed;
      access_methods += other.access_methods;
      hot_src_block_count += other.hot_src_block_count;
      cold_src_block_count += other.cold_src_block_count;
      hot_throw_cold_block_count += other.hot_throw_cold_block_count;
      normalized_blocks += other.normalized_blocks;
      denormalized_blocks += other.denormalized_blocks;
      elided_vals += other.elided_vals;
      unelided_vals += other.unelided_vals;
      return *this;
    }
  };

  InsertResult insert_source_blocks_into_method(
      DexMethod* method,
      std::vector<const DexMethodRef*>& failed_methods,
      SimpleSMIStore& smi,
      std::mutex& failed_methods_mutex,
      bool serialize,
      bool exc_inject,
      int32_t block_appear100_threshold,
      bool must_be_cold = false) {
    auto* code = method->get_code();
    if (code != nullptr) {
      auto access_method = is_traditional_access_method(method);
      const DexType* access_method_type = nullptr;
      std::string_view access_method_name;
      std::string access_method_hash_name;

      always_assert(code->cfg_built());
      auto& cfg = code->cfg();
      if (access_method) {
        access_method_type = access_method->first;
        access_method_name = access_method->second;

        auto hash_value = hasher::stable_hash(cfg);

        access_method_hash_name =
            hasher::hashed_name(hash_value, access_method_name);
      }

      const auto* sb_name = [&]() {
        if (!access_method) {
          return &method->get_deobfuscated_name();
        }
        // Emulate show.
        std::string new_name = show_deobfuscated(method->get_class());
        new_name.append(".access$");
        new_name.append(access_method_hash_name);
        new_name.append(":");
        new_name.append(show_deobfuscated(method->get_proto()));
        return DexString::make_string(new_name);
      }();

      source_blocks::InsertResult res;
      // NOLINTBEGIN(facebook-hte-NullableDereference)
      auto profiles =
          find_profiles(method, access_method_type, access_method_name,
                        access_method_hash_name);
      // NOLINTEND(facebook-hte-NullableDereference)
      if (!profiles.second && !always_inject) {
        // Skip without profile.
        return InsertResult(access_method ? 1 : 0, 1);
      }

      if (use_default_value || use_fuzzing_values) {

        res = source_blocks::insert_custom_source_blocks(
            sb_name, &cfg, profiles.first, serialize, exc_inject,
            use_fuzzing_values, must_be_cold);
      } else {
        res = source_blocks::insert_source_blocks(sb_name, &cfg, profiles.first,
                                                  serialize, exc_inject);
      }

      if (fix_violations) {
        source_blocks::fix_hot_method_cold_entry_violations(&cfg);
        source_blocks::fix_chain_violations(&cfg);
        source_blocks::fix_idom_violations(&cfg);
      }

      if (block_appear100_threshold > 0) {
        always_assert(block_appear100_threshold <= 100);
        source_blocks::adjust_block_hits_with_appear100_threshold(
            &cfg, block_appear100_threshold);
      }

      smi.add({sb_name, std::move(res.serialized),
               std::move(res.serialized_idom_map)});

      if (!res.profile_success) {
        std::unique_lock<std::mutex> lock{failed_methods_mutex};
        failed_methods.push_back(method);
      }

      auto source_block_metrics =
          source_blocks::gather_source_block_metrics(&cfg);
      size_t hot_src_block_current_count = source_block_metrics.hot_block_count;
      size_t cold_src_block_current_count =
          source_block_metrics.cold_block_count;
      size_t hot_throw_cold_block_count =
          source_block_metrics.hot_throw_cold_count;

      return InsertResult(
          access_method ? 1 : 0, res.block_count, profiles.second ? 1 : 0,
          res.profile_success ? 0 : 1, hot_src_block_current_count,
          cold_src_block_current_count, hot_throw_cold_block_count,
          res.normalized_count, res.denormalized_count, res.elided_vals,
          res.unelided_vals);
    }
    return InsertResult();
  }

  InsertResult run_fuzzing_on_source_blocks(
      const Scope& scope,
      std::vector<const DexMethodRef*>& failed_methods,
      SimpleSMIStore& smi,
      std::mutex& failed_methods_mutex,
      bool serialize,
      bool exc_inject) {
    auto method_override_graph = method_override_graph::build_graph(scope);
    auto call_graph = std::make_unique<call_graph::Graph>(
        call_graph::single_callee_graph(*method_override_graph, scope));

    UnorderedMap<call_graph::NodeId, MethodFuzzingMetadata> method_metadata;
    UnorderedMap<IRInstruction*, bool> caller_hit_lookup;
    // Set up and count indegrees
    source_blocks::impl::visit_by_levels(
        &*call_graph, [&](call_graph::NodeId node) {
          if (method_metadata.find(node) == method_metadata.end()) {
            method_metadata.insert({node, MethodFuzzingMetadata(0, 0)});
          }
          method_metadata.at(node).indegrees = node->callers().size();
        });

    InsertOnlyConcurrentSet<DexMethod*> seen_methods;
    InsertResult res;
    topo_traverse_callgraph(
        method_metadata, *call_graph, [&](call_graph::NodeId node) {
          if (node->is_entry() || node->is_exit() ||
              (node->method() == nullptr)) {
            return;
          }
          bool must_be_cold = false;
          bool all_cold_callers = true;
          bool seen_caller = false;
          // Checks all the callers to see if there is at least one hot source
          // block before the invoke instruction, if there is, then the callee
          // is hot
          for (const auto& edge : node->callers()) {
            const auto* caller = edge->caller();
            if (caller->is_entry() || caller->is_exit() ||
                (caller->method() == nullptr)) {
              continue;
            }
            auto* caller_invoke_insn = edge->invoke_insn();
            if (caller_hit_lookup.find(caller_invoke_insn) ==
                caller_hit_lookup.end()) {
              continue;
            }
            seen_caller = true;
            if (caller_hit_lookup.at(caller_invoke_insn)) {
              // At least one caller has a hot source block before the invoke
              all_cold_callers = false;
            }
          }

          must_be_cold = (seen_caller && all_cold_callers);
          auto* method = const_cast<DexMethod*>(node->method());
          res += insert_source_blocks_into_method(
              method, failed_methods, smi, failed_methods_mutex, serialize,
              exc_inject, 0, must_be_cold);
          seen_methods.insert(method);

          // Update the caller_hit_lookup map with the hit status of the
          // block with new source blocks
          auto* code = method->get_code();
          if (code != nullptr) {
            auto& cfg = code->cfg();
            for (auto* block : cfg.blocks()) {
              SourceBlock* prev_sb = nullptr;
              for (auto it = block->begin(); it != block->end(); it++) {
                if (it->type == MFLOW_OPCODE) {
                  if (opcode::is_an_invoke(it->insn->opcode())) {
                    if ((prev_sb != nullptr) && prev_sb->vals_size > 0) {
                      auto* invoke_insn = it->insn;
                      bool hit = prev_sb->get_val(0).value_or(0) > 0;
                      caller_hit_lookup[invoke_insn] =
                          std::max(caller_hit_lookup[invoke_insn], hit);
                    }
                  }
                }

                if (it->type == MFLOW_SOURCE_BLOCK) {
                  prev_sb = it->src_block.get();
                }
              }
            }
          }
        });

    // The call graph may not contain every single method possible, therefore
    // a loop over all methods in the scope is needed again to fill in the
    // source blocks of methods that were not seen in the call graph
    res += walk::parallel::methods<InsertResult>(scope, [&](DexMethod* method) {
      if (seen_methods.count(method) == 0) {
        return insert_source_blocks_into_method(method, failed_methods, smi,
                                                failed_methods_mutex, serialize,
                                                exc_inject, 0);
      } else {
        return InsertResult();
      }
    });
    return res;
  }

  void run_source_blocks(DexStoresVector& stores,
                         PassManager& mgr,
                         bool serialize,
                         bool exc_inject,
                         int32_t block_appear100_threshold) {
    auto scope = build_class_scope(stores);

    SimpleSMIStore smi{};

    std::vector<const DexMethodRef*> failed_methods{};
    std::mutex failed_methods_mutex{};
    failed_methods.reserve(10000);

    InsertResult res;
    if (use_fuzzing_values && !use_default_value) {
      // This path is used for fuzzing
      res = run_fuzzing_on_source_blocks(scope, failed_methods, smi,
                                         failed_methods_mutex, serialize,
                                         exc_inject);
    } else {
      res =
          walk::parallel::methods<InsertResult>(scope, [&](DexMethod* method) {
            return insert_source_blocks_into_method(
                method, failed_methods, smi, failed_methods_mutex, serialize,
                exc_inject, block_appear100_threshold);
          });
    }

    if (conf.get_global_config()
            .get_config_by_name<AssessorConfig>("assessor")
            ->run_sb_consistency) {
      source_blocks::get_sbcc().initialize(scope);
    }

    mgr.set_metric("inserted_source_blocks", res.blocks);
    mgr.set_metric("handled_methods", smi.data.size());
    mgr.set_metric("skipped_methods", res.skipped);
    mgr.set_metric("methods_with_profiles", res.profile_count);
    mgr.set_metric("profile_failed", res.profile_failed);
    mgr.set_metric("access_methods", res.access_methods);
    mgr.set_metric("hot_source_block_count", res.hot_src_block_count);
    mgr.set_metric("cold_source_block_count", res.cold_src_block_count);
    mgr.set_metric("hot_throw_cold_block_count",
                   res.hot_throw_cold_block_count);
    mgr.set_metric("normalized_blocks", res.normalized_blocks);
    mgr.set_metric("denormalized_blocks", res.denormalized_blocks);
    mgr.set_metric("elided_vals", res.elided_vals);
    mgr.set_metric("unelided_vals", res.unelided_vals);
    {
      size_t unresolved = 0;
      for (const auto& p_file : profile_files) {
        unresolved += p_file->unresolved_methods.size();
      }
      mgr.set_metric("avg_unresolved_methods_100",
                     unresolved > 0
                         ? (int64_t)(unresolved * 100.0 / profile_files.size())
                         : 0);
    }

    if (!failed_methods.empty()) {
      write_sorted_methods(conf.metafile("redex-isb-failed-methods.txt"),
                           failed_methods);
    }

    if (!serialize) {
      return;
    }

    // Put all unique idom maps into a file.
    std::vector<std::string> unique_idom_maps;
    {
      std::set<std::string> unique_idom_maps_set;
      std::transform(
          smi.data.begin(), smi.data.end(),
          std::inserter(unique_idom_maps_set, unique_idom_maps_set.begin()),
          [](const auto& s) { return s.idom_map; });
      unique_idom_maps.reserve(unique_idom_maps_set.size());
      unique_idom_maps.insert(unique_idom_maps.end(),
                              unique_idom_maps_set.begin(),
                              unique_idom_maps_set.end());
    }

    std::ofstream ofs_uim(conf.metafile("unique-idom-maps.txt"));
    for (const auto& uim : unique_idom_maps) {
      ofs_uim << uim << "\n";
    }

    std::sort(smi.data.begin(), smi.data.end(),
              [](const auto& lhs, const auto& rhs) {
                return compare_dexstrings(lhs.method, rhs.method);
              });

    std::ofstream ofs_rsb(conf.metafile("redex-source-blocks.csv"));
    ofs_rsb << "type,version\nredex-source-blocks,1\nname,serialized\n";

    std::ofstream ofs_rsbidm(conf.metafile("redex-source-block-idom-maps.csv"));
    ofs_rsbidm
        << "type,version\nredex-source-blocks-idom-maps,1\nidom_map_id\n";

    for (const auto& [method, s_expression, idom_map] : smi.data) {
      ofs_rsb << show(method) << "," << s_expression << "\n";

      // idom_map_id is a line index into unique-idom-maps.txt
      auto it = std::lower_bound(unique_idom_maps.begin(),
                                 unique_idom_maps.end(), idom_map);
      always_assert(it != unique_idom_maps.end() && *it == idom_map);
      size_t idom_map_id = std::distance(unique_idom_maps.begin(), it);
      ofs_rsbidm << idom_map_id << "\n";
    }
  }

  template <typename T, typename Fn, typename Pred>
  void sort_and_set_indices(T& container, const Fn& fn, const Pred& pred) {
    std::sort(container.begin(), container.end(),
              [&fn, &pred](const auto& lhs_in, const auto& rhs_in) {
                if (lhs_in == rhs_in) {
                  return false;
                }
                const auto& lhs = fn(lhs_in);
                const auto& rhs = fn(rhs_in);
                if (lhs == rhs) {
                  return false;
                }
                return pred(lhs, rhs);
              });

    UnorderedMap<std::string, size_t> interaction_indices;
    for (size_t i = 0; i != container.size(); ++i) {
      interaction_indices[fn(container[i])] = i;
    }
    g_redex->set_sb_interaction_index(interaction_indices);
  }

  static void write_sorted_methods(const std::string& fname,
                                   std::vector<const DexMethodRef*>& methods) {
    std::sort(methods.begin(), methods.end(), compare_dexmethods);

    std::ofstream ofs{fname};
    for (const auto* mref : methods) {
      ofs << show(mref) << "\n";
    }
  }

  void prepare_profile_files_and_interactions(
      const std::string& profile_files_str,
      const std::vector<std::string>& ordered_interactions) {
    UnorderedMap<std::string, size_t> ordered_interactions_indices;
    for (const auto& s : ordered_interactions) {
      ordered_interactions_indices.emplace(s,
                                           ordered_interactions_indices.size());
    }
    auto get_interaction_index =
        [&](const std::string& interaction_id) -> size_t {
      auto it = ordered_interactions_indices.find(interaction_id);
      return it == ordered_interactions_indices.end()
                 ? std::numeric_limits<size_t>::max()
                 : it->second;
    };
    auto interaction_less = [&](const std::string& lhs,
                                const std::string& rhs) -> bool {
      auto lhs_index = get_interaction_index(lhs);
      auto rhs_index = get_interaction_index(rhs);
      if (lhs_index != rhs_index) {
        return lhs_index < rhs_index;
      }
      return lhs < rhs;
    };

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
      sort_and_set_indices(
          profile_files,
          [](const auto& u) -> const std::string& { return u->interaction; },
          interaction_less);

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
        sort_and_set_indices(
            interactions, [](const auto& s) -> const std::string& { return s; },
            interaction_less);
      }
    }
  }

  void write_unresolved_methods(const std::string& fname) const {
    // Using a set to avoid hashing all of it. Similar approach to RedexContext.
    // Assumption is set is small overall. Also helps for sorting strings.
    std::set<std::string_view> unresolved_uniqued;
    for (const auto& p : profile_files) {
      insert_unordered_iterable(unresolved_uniqued, p->unresolved_methods);
    }
    std::ofstream ofs{fname};
    for (const auto& sv : unresolved_uniqued) {
      ofs << sv << "\n";
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
  bind("default_value", m_use_default_value, m_use_default_value,
       "Use a default value for the inserted source blocks. The default value "
       "is defined in SourceBlocks.cpp");
  bind("ordered_interactions", {"ColdStart"}, m_ordered_interactions);
  bind("fix_violations",
       m_fix_violations,
       m_fix_violations,
       "Applies best effort fix to all source block violations.");
  bind("enable_source_block_fuzzing", m_enable_source_block_fuzzing,
       m_enable_source_block_fuzzing,
       "When enabled, applies fuzzing to inserted source block");
  bind("block_appear100_threshold", m_block_appear100_threshold,
       m_block_appear100_threshold,
       "Block appear100 threshold configuration (0-100)");

  if (m_block_appear100_threshold > 100) {
    always_assert_log(false, "block_appear100_threshold must be <= 100, got %d",
                      m_block_appear100_threshold);
  }
}

void InsertSourceBlocksPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  bool is_instr_mode = mgr.get_redex_options().instrument_pass_enabled;
  bool always_inject = m_always_inject || m_force_serialize || is_instr_mode;

  Injector inj(conf, always_inject, m_use_default_value,
               m_enable_source_block_fuzzing, m_fix_violations);

  inj.prepare_profile_files_and_interactions(m_profile_files,
                                             m_ordered_interactions);
  inj.write_unresolved_methods(
      conf.metafile("redex-isb-unresolved-methods.txt"));

  inj.run_source_blocks(stores, mgr,
                        /* serialize= */ m_force_serialize || is_instr_mode,
                        m_insert_after_excs, m_block_appear100_threshold);

  for (auto&& [interaction_id, index] :
       UnorderedIterable(g_redex->get_sb_interaction_indices())) {
    mgr.set_metric("interaction_" + interaction_id, index);
  }

  {
    Timer timer("Compute method violations");
    auto scope = build_class_scope(stores);
    auto method_override_graph = method_override_graph::build_graph(scope);
    auto call_graph = std::make_unique<call_graph::Graph>(
        call_graph::single_callee_graph(*method_override_graph, scope));

    auto val = source_blocks::compute_method_violations(*call_graph, scope);
    mgr.set_metric("method~violation~hot~callee~cold~callers", val);
  }
}

static InsertSourceBlocksPass s_pass;
