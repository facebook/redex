/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexVt.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <unordered_map>

#include <boost/io/quoted.hpp>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BaselineProfile.h"
#include "BlockOffsetSink.h"
#include "CallGraph.h"
#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "Debug.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "IROpcode.h"
#include "MethodOverrideGraph.h"
#include "MethodProfiles.h"
#include "RedexContext.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"
#include "Walkers.h"

namespace dexvt {

namespace {
// The deobfuscated method name is fully-qualified ("Lclass;.name:proto"); strip
// the class prefix so the record's `method` field holds just "name:proto"
// (class is a separate field; the full canonical id is `obfuscated_name`).
std::string bare_method(const std::string& full_deobf) {
  auto pos = full_deobf.find(";.");
  return pos == std::string::npos ? full_deobf : full_deobf.substr(pos + 2);
}

// Replace TAB/newline so a value is safe to embed in a TSV/ctags field.
std::string tsv_safe(std::string s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return s;
}

// Short lowercase word for a CFG edge type (the vocabulary of ControlFlow's
// Edge printer, minus its pointer-bearing verbosity).
std::string_view edge_word(cfg::EdgeType t) {
  switch (t) {
  case cfg::EDGE_GOTO:
    return "goto";
  case cfg::EDGE_BRANCH:
    return "branch";
  case cfg::EDGE_THROW:
    return "throw";
  case cfg::EDGE_GHOST:
    return "ghost";
  case cfg::EDGE_TYPE_SIZE:
    break; // sentinel, not a real edge
  }
  return "edge";
}

// Deterministic, diffable CFG text. redex's show(cfg, code_only) prefixes every
// line with a live MethodItemEntry heap address (Show.cpp: `o << "[" << &mie`),
// which is non-reproducible across runs and breaks byte-stable/diffable output.
// Render our own from the same CFG: blocks in ascending id order, opcode lines
// only (via the pointer-free show(insn)), and branch structure as a sorted
// successor list -- targets are CFG edges, never inline in the instruction
// text.
std::string render_disasm(const cfg::ControlFlowGraph& cfg) {
  std::vector<cfg::Block*> blocks = cfg.blocks();
  std::sort(blocks.begin(), blocks.end(),
            [](const cfg::Block* a, const cfg::Block* b) {
              return a->id() < b->id();
            });
  const cfg::Block* entry = cfg.entry_block();
  std::ostringstream ss;
  for (cfg::Block* b : blocks) {
    ss << 'B' << b->id() << (b == entry ? ": entry\n" : ":\n");
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_OPCODE) {
        const IRInstruction* insn = mie.insn;
        if (insn->has_string()) {
          // Dex strings are MUTF-8: a supplementary char (e.g. an emoji) is a
          // surrogate pair, invalid as UTF-8, so the default show() emits bytes
          // the NDJSON writer then mangles to U+FFFD. Re-render the trailing
          // string operand via show_escaped, which decodes MUTF-8 -> proper
          // UTF-8.
          const std::string line = show(insn);
          const auto q = line.find('"');
          ss << "  " << (q == std::string::npos ? line : line.substr(0, q));
          if (q != std::string::npos) {
            ss << boost::io::quoted(show_escaped(insn->get_string()));
          }
          ss << '\n';
        } else {
          ss << "  " << show(insn) << '\n';
        }
      }
    }
    std::vector<std::pair<size_t, std::string_view>> succs;
    for (const cfg::Edge* e : b->succs()) {
      if (e->target() != nullptr) {
        succs.emplace_back(e->target()->id(), edge_word(e->type()));
      }
    }
    std::sort(succs.begin(), succs.end());
    if (!succs.empty()) {
      ss << "  ->";
      for (const auto& [target_id, word] : succs) {
        ss << ' ' << word << " B" << target_id;
      }
      ss << '\n';
    }
  }
  return ss.str();
}
} // namespace

void Exporter::capture_pre_lowering(DexStoresVector& stores,
                                    ConfigFiles& conf) {
  auto scope = build_class_scope(stores);

  // 1) The class -> "<store>/<dex_index>" map from the finalized InterDex split
  // (dexen are final by the backend), so records can be grouped by dex file.
  UnorderedMap<const DexClass*, std::string> class_dex;
  for (auto& store : stores) {
    const auto& dexen = store.get_dexen();
    for (size_t di = 0; di < dexen.size(); ++di) {
      const std::string tag = store.get_name() + "/" + std::to_string(di);
      for (auto* cls : dexen[di]) {
        class_dex.emplace(cls, tag);
      }
    }
  }

  // Deterministic id assignment (serial, stable order) reused by emit():
  // methods first, then fields (static then instance per class), all in one id
  // space.
  uint32_t next_id = 0;
  for (auto* cls : scope) {
    for (auto* m : cls->get_dmethods()) {
      m_ids.emplace(m, next_id++);
    }
    for (auto* m : cls->get_vmethods()) {
      m_ids.emplace(m, next_id++);
    }
  }
  for (auto* cls : scope) {
    auto it = class_dex.find(cls);
    const std::string dx = it != class_dex.end() ? it->second : std::string();
    for (auto* f : cls->get_sfields()) {
      m_fields.push_back(FieldRecord{f, next_id++, dx});
    }
    for (auto* f : cls->get_ifields()) {
      m_fields.push_back(FieldRecord{f, next_id++, dx});
    }
  }

  // Class inheritance edges (deobfuscated super + directly-implemented
  // interfaces). Subclasses/implementors are derived on the read side by
  // inverting these, so only the forward edges are captured here.
  auto deobf_type = [](const DexType* t) -> std::string {
    auto* c = type_class(t);
    return c != nullptr ? c->get_deobfuscated_name_or_empty_copy() : show(t);
  };
  m_classes.reserve(scope.size());
  for (auto* cls : scope) {
    ClassRecord cr;
    cr.name = cls->get_deobfuscated_name_or_empty_copy();
    if (auto* super = cls->get_super_class(); super != nullptr) {
      cr.super = deobf_type(super);
    }
    for (const auto* intf : *cls->get_interfaces()) {
      cr.interfaces.push_back(deobf_type(intf));
    }
    m_classes.push_back(std::move(cr));
  }

  // 2) SourceBlock interaction index -> name (its own index space, distinct
  // from the MethodProfiles interaction ids).
  m_sb_interaction_names.assign(g_redex->num_sb_interaction_indices(),
                                std::string());
  for (auto&& [name, idx] :
       UnorderedIterable(g_redex->get_sb_interaction_indices())) {
    if (idx < m_sb_interaction_names.size()) {
      m_sb_interaction_names[idx] = name;
    }
  }

  // 3) Betamap coldstart rank: position of each real class in the coldstart
  // order. The order list interleaves marker sentinels (dex boundaries, pct-end
  // markers, interaction sections) that are synthetic and do not resolve to a
  // loaded class, so skipping types with no DexClass drops them cleanly.
  UnorderedMap<const DexType*, uint32_t> coldstart_rank;
  {
    uint32_t rank = 0;
    for (const auto& name : conf.get_coldstart_classes()) {
      auto* type = DexType::get_type(name);
      if (type == nullptr || type_class(type) == nullptr) {
        continue;
      }
      coldstart_rank.emplace(type, rank++);
    }
  }

  // Coarse InterDex grouping (lazily built; fetch once before the parallel
  // walk).
  const auto& interdex_groups = conf.get_cls_interdex_groups();
  const bool has_interdex_groups = conf.get_num_interdex_groups() > 0;

  // ART baseline profile (default/driver-input profile), if configured.
  const auto& mp = conf.get_method_profiles();
  for (const auto& interaction : mp.all_interactions()) {
    m_pgo_interactions.push_back(interaction.first);
  }
  // Sort for a deterministic manifest: all_interactions() iteration order is
  // not guaranteed stable across runs (consumers join by name, so order is
  // free).
  std::sort(m_pgo_interactions.begin(), m_pgo_interactions.end());
  m_baseline_available = !conf.get_baseline_profile_configs().empty();
  baseline_profiles::BaselineProfile baseline;
  if (m_baseline_available) {
    baseline = baseline_profiles::get_default_baseline_profile(
        scope, conf.get_baseline_profile_configs(), mp);
  }

  // Method -> callers xref. complete_call_graph folds MethodOverrideGraph
  // resolution into the caller edges, so virtual/interface callers are
  // complete.
  auto override_graph = method_override_graph::build_graph(scope);
  auto callgraph = call_graph::complete_call_graph(*override_graph, scope);

  // 4) Parallel per-method capture. The CFG was cleared by PassManager before
  // the backend, so ScopedCFG rebuilds it here and tears it back down on scope
  // exit, leaving the linear IR as instruction_lowering expects it.
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    MethodRecord rec;
    rec.id = m_ids.at(method);

    cfg::ScopedCFG scoped_cfg(&code);
    // Deterministic in-house CFG text (see render_disasm); hotness is captured
    // structurally in rec.blocks rather than double-encoded in the text.
    rec.disasm = render_disasm(code.cfg());

    UnorderedMap<const MethodItemEntry*, uint32_t> leaders;
    for (auto* block : code.cfg().blocks()) {
      // Anchor each block by its first DEX-EMITTING opcode MethodItemEntry ->
      // cfg block id. An instruction entry lowers in place (dex_insn set on the
      // SAME object) and splices through CFG teardown with stable pointer
      // identity, so IRCode::sync resolves it to the block's final code-unit
      // offset.
      //
      // Two entry kinds must NOT be the anchor, both because they fail to reach
      // sync -- leaving the block with no offset at all:
      //
      //  - &*block->begin(): for a source-blocked block that is a leading
      //    MFLOW_SOURCE_BLOCK, whose MIE does not survive to sync (exactly the
      //    hot, source-blocked blocks we care most about).
      //  - any INTERNAL opcode (IOPCODE_*): these emit no DEX, so anchoring on
      //    one records the address of whatever follows. A block whose only
      //    opcodes are internal then aliases the next block's start_cu, and the
      //    dex-pc -> block lookup resolves the tie arbitrarily. In particular:
      //  - a load-param (IOPCODE_LOAD_PARAM*): for a NON-STATIC method,
      //    instruction_lowering's check_load_params erase_and_dispose()s the
      //    leading `this` load-param outright, so the captured pointer dangles
      //    and its block -- always the ENTRY block -- silently loses its
      //    offset. (A static method's load-params are merely retyped in place
      //    to MFLOW_FALLTHROUGH by remove_opcode, so anchoring one happens to
      //    work there -- an accident we must not rely on. Worse, a disposed
      //    MIE's address can be reused by a later allocation and produce a
      //    WRONG match.) Load-params emit no DEX, so the block's true start_cu
      //    is its first real opcode either way.
      //
      // INVARIANT this relies on: nothing runs between here and DexOutput's
      // sync pass except instruction lowering. An anchor is a raw
      // MethodItemEntry*, so a pass that DISPOSED an anchored opcode would
      // leave it dangling -- and because the lookup only compares the pointer,
      // a reused address yields a wrong offset rather than a crash. Lowering is
      // the only such pass today and load-params are the only entries it
      // disposes, which is why they are skipped above. Anything inserted into
      // that window must either preserve anchored opcodes or re-run the
      // capture.
      //
      // Non-opcode entries before the first opcode share its code-unit address,
      // so the recorded start_cu is still the true block start. Anchoring every
      // block -- not only source-blocked ones -- lets a block's three levels
      // (IR/true-DEX/native) slice to exactly the same region. A block with no
      // DEX-emitting opcode is skipped: it has no DEX to show and its offset
      // would alias the next block's start_cu.
      const MethodItemEntry* first_op = nullptr;
      for (const auto& mie : *block) {
        if (mie.type == MFLOW_OPCODE &&
            !opcode::is_an_internal(mie.insn->opcode())) {
          first_op = &mie;
          break;
        }
      }
      if (first_op != nullptr) {
        leaders.emplace(first_op, static_cast<uint32_t>(block->id()));
      }
      // Source-blocked blocks additionally carry hotness.
      const auto* sb = source_blocks::get_first_source_block(block);
      if (sb == nullptr) {
        continue;
      }
      for (size_t i = 0; i < sb->vals_size; ++i) {
        auto val = sb->get_val(i);
        auto appear = sb->get_appear100(i);
        float v = val ? *val : 0.f;
        float a = appear ? *appear : 0.f;
        if (v == 0.f && a == 0.f) {
          continue; // sparse: omit the default (0,0) sample
        }
        rec.blocks.push_back(BlockHotness{static_cast<uint32_t>(block->id()),
                                          static_cast<uint32_t>(i), v, a});
      }
    }
    if (!leaders.empty()) {
      block_offset_sink::set_leaders(method, std::move(leaders));
    }

    for (const auto& interaction : mp.all_interactions()) {
      const std::string& interaction_id = interaction.first;
      auto stat = mp.get_method_stat(interaction_id, method);
      if (!stat) {
        continue;
      }
      rec.pgo.push_back(MethodPgoStat{interaction_id, stat->appear_percent,
                                      stat->call_count, stat->order_percent});
    }

    auto* cls_type = method->get_class();
    if (auto it = coldstart_rank.find(cls_type); it != coldstart_rank.end()) {
      rec.betamap_rank = static_cast<int64_t>(it->second);
    }
    if (has_interdex_groups) {
      if (auto it = interdex_groups.find(cls_type);
          it != interdex_groups.end()) {
        rec.interdex_group = static_cast<int64_t>(it->second);
      }
    }
    if (auto it = baseline.methods.find(method); it != baseline.methods.end()) {
      rec.baseline_hot = it->second.hot;
      rec.baseline_startup = it->second.startup;
      rec.baseline_post_startup = it->second.post_startup;
    }

    if (callgraph.has_node(method)) {
      for (const auto* caller :
           UnorderedIterable(callgraph.get_callers(method))) {
        if (caller == nullptr) {
          continue;
        }
        if (auto it = m_ids.find(caller); it != m_ids.end()) {
          rec.callers.push_back(it->second);
        }
      }
      std::sort(rec.callers.begin(), rec.callers.end());
    }

    if (auto* dcls = type_class(cls_type); dcls != nullptr) {
      if (auto it = class_dex.find(dcls); it != class_dex.end()) {
        rec.dex = it->second;
      }
    }

    m_records.emplace(method, std::move(rec));
  });

  // Enable the block-offset sink so IRCode::sync (driven later by DexOutput)
  // records each SourceBlock's final code-unit offset; emit() drains it.
  block_offset_sink::enable();

  TRACE(MAIN,
        1,
        "[dexvt] pre-lowering captured %zu methods (%zu SB interactions)",
        m_records.size(),
        m_sb_interaction_names.size());
}

void Exporter::emit(ConfigFiles& conf,
                    const enhanced_dex_stats_t& output_totals) {
  const std::string path = conf.metafile("redex-dexvt.ndjson");
  std::ofstream ofs(path);
  always_assert_log(ofs.is_open(), "dexvt: failed to open %s for writing",
                    path.c_str());

  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  // Hotness/PGO floats (SourceBlock val/appear, MethodProfiles percents) carry
  // ~15 meaningless digits; 7 significant figures is well beyond real precision
  // and cuts ~17-char literals to ~5. Ints (ids, sizes, access) are unaffected.
  builder["precision"] = 7;
  builder["precisionType"] = "significant";
  auto writer = std::unique_ptr<Json::StreamWriter>(builder.newStreamWriter());

  // Join-key validation: the pre-lowering capture and the post-lowering sizes
  // are joined by the interned DexMethod*. Every sized method must have a
  // captured id, otherwise identity drifted across lowering and records would
  // mis-associate sizes with bodies.
  size_t sized_without_id = 0;
  for (auto&& entry : UnorderedIterable(output_totals.method_size)) {
    if (m_ids.find(entry.first) == m_ids.end()) {
      ++sized_without_id;
    }
  }
  // Degrade, don't abort: --emit-dexvt is an opt-in diagnostic, so a method
  // that appeared after capture (identity drift) just loses its size in the
  // artifact rather than crashing the whole redex build. Expected to be 0 today
  // (capture runs after PassManager, before instruction_lowering).
  if (sized_without_id != 0) {
    TRACE(MAIN, 1,
          "[dexvt] %zu post-lowering sized methods had no captured id "
          "(DexMethod* join key drifted across lowering); sizes omitted",
          sized_without_id);
  }
  if (!m_records.empty() && output_totals.method_size.empty()) {
    // Degrade, don't abort (see above): emit the artifact without sizes rather
    // than crashing the build for an opt-in diagnostic.
    TRACE(MAIN, 1,
          "[dexvt] no per-method sizes captured for %zu records; sizes omitted",
          m_records.size());
  }

  // Deterministic order by the stable id (reproducible / diffable output).
  // Built before the manifest so the interned caller-set count is known when it
  // is written.
  std::vector<const DexMethod*> order;
  order.reserve(m_records.size());
  for (auto&& entry : UnorderedIterable(m_records)) {
    order.push_back(entry.first);
  }
  std::sort(order.begin(), order.end(),
            [this](const DexMethod* a, const DexMethod* b) {
              return m_ids.at(a) < m_ids.at(b);
            });

  // Intern caller-sets: the override-resolved call graph emits O(overrides x
  // callsites) IDENTICAL caller lists, so store each distinct (already
  // sorted+unique) list once in redex-dexvt.callersets.tsv and reference it by
  // a dense set_id. First-seen over the id-sorted `order` makes the set_ids
  // deterministic; &it->first is a stable std::map key pointer.
  std::map<std::vector<uint32_t>, uint32_t> caller_set_ids;
  std::vector<const std::vector<uint32_t>*> sets_by_id;
  for (const DexMethod* method : order) {
    const MethodRecord& rec = m_records.at_unsafe(method);
    if (rec.callers.empty()) {
      continue;
    }
    auto [it, inserted] = caller_set_ids.emplace(
        rec.callers, static_cast<uint32_t>(sets_by_id.size()));
    if (inserted) {
      sets_by_id.push_back(&it->first);
    }
  }

  // Manifest (first line): schema + the SourceBlock interaction index->name map
  // so consumers can label the blocks[] columns.
  Json::Value manifest;
  manifest["record"] = "manifest";
  manifest["schema_version"] =
      // This is the on-disk EXPORT format version, independent of the derived
      // SQLite catalog's `sqlite._USER_VERSION` -- the two count separately and
      // are expected to differ (see facebook/dexvt/ARCHITECTURE.md).
      8; // v8 carries block_offsets for every block with code, entry block
         // included, so a block's three levels slice to the same region; v7
         // and v6 carry them for a narrower set of blocks; v5 adds
         // kind:"class" records (super + interfaces); v4 interns caller lists
         // into callersets.tsv, referenced per method via caller_set
  manifest["method_count"] = static_cast<Json::UInt64>(m_records.size());
  manifest["field_count"] = static_cast<Json::UInt64>(m_fields.size());
  manifest["class_count"] = static_cast<Json::UInt64>(m_classes.size());
  manifest["callerset_count"] = static_cast<Json::UInt64>(sets_by_id.size());
  Json::Value sb_interactions(Json::arrayValue);
  for (const auto& name : m_sb_interaction_names) {
    sb_interactions.append(name);
  }
  manifest["sb_interactions"] = sb_interactions;
  Json::Value pgo_interactions(Json::arrayValue);
  for (const auto& id : m_pgo_interactions) {
    pgo_interactions.append(id);
  }
  manifest["pgo_interactions"] = pgo_interactions;
  manifest["proguard_map"] =
      conf.get_json_config().get("proguard_map", std::string());
  manifest["callgraph"] = "complete";
  manifest["baseline_available"] = m_baseline_available;
  // Method-profile resolution health (raw counts; size() and unresolved_size()
  // have different denominators, so do not divide them). Low unresolved => the
  // profile<->dex join landed.
  const auto& mp = conf.get_method_profiles();
  manifest["pgo_resolved"] = static_cast<Json::UInt64>(mp.size());
  manifest["pgo_unresolved"] = static_cast<Json::UInt64>(mp.unresolved_size());
  writer->write(manifest, &ofs);
  ofs << "\n";

  // Skeleton entries (one per method, id-sorted) drive the O(1) seek sidecar
  // and the line-addressable ctags skeleton written after the body loop.
  struct SkelEntry {
    uint32_t id;
    uint64_t byte_off; // offset of the body record within the NDJSON
    uint64_t code_item_size;
    std::string obf; // precomputed, tsv-safe -- works for methods and fields
    std::string deobf_class;
    std::string deobf_member;
    char kind; // 'm' method, 'f' field
  };
  std::vector<SkelEntry> skeleton;
  skeleton.reserve(order.size() + m_fields.size());

  for (const DexMethod* method : order) {
    const MethodRecord& rec = m_records.at_unsafe(method);
    Json::Value r;
    r["id"] = static_cast<Json::UInt>(rec.id);
    r["obfuscated_name"] = show(method); // canonical cross-artifact join key
    r["method"] =
        bare_method(method->get_deobfuscated_name_or_empty_copy()); // display
    if (auto* cls = type_class(method->get_class()); cls != nullptr) {
      r["class"] = cls->get_deobfuscated_name_or_empty_copy();
    } else {
      r["class"] = show(method->get_class());
    }
    r["access"] = static_cast<Json::UInt>(method->get_access());
    r["kind"] = "method";
    if (!rec.dex.empty()) {
      r["dex"] = rec.dex;
    }

    if (auto it = output_totals.method_size.find(method);
        it != output_totals.method_size.end()) {
      r["code_item_size"] = static_cast<Json::UInt64>(it->second);
    }
    if (!rec.disasm.empty()) {
      r["disasm"] = rec.disasm;
    }
    if (!rec.blocks.empty()) {
      // Columnar (struct-of-arrays): the 4 keys are spelled once per method
      // instead of once per sample, and `ix` is the SourceBlock interaction
      // index (resolve via manifest `sb_interactions`) rather than the name
      // re-spelled every sample.
      Json::Value blk(Json::arrayValue), ix(Json::arrayValue),
          val(Json::arrayValue), a100(Json::arrayValue);
      for (const auto& b : rec.blocks) {
        blk.append(static_cast<Json::UInt>(b.block_id));
        ix.append(static_cast<Json::UInt>(b.interaction_idx));
        val.append(b.val);
        a100.append(b.appear100);
      }
      Json::Value blocks(Json::objectValue);
      blocks["blk"] = blk;
      blocks["ix"] = ix;
      blocks["val"] = val;
      blocks["a100"] = a100;
      r["blocks"] = blocks;
    }
    // block_offsets: [blk, start_code_unit] for EVERY cfg block. The
    // BlockOffsetSink resolved each block's first-MethodItemEntry anchor
    // (registered pre-lowering) to its final code-unit offset during
    // IRCode::sync, so every block -- source-blocked or not -- is addressable
    // by its shipped-DEX offset and its three levels (IR/true-DEX/native) slice
    // to exactly the same region. Sorted by (offset, blk) to match the read
    // side. Empty unless a sync pass ran under the enabled sink (the real
    // backend, or a driving test).
    if (const auto* offs = block_offset_sink::get(method); offs != nullptr) {
      std::vector<std::pair<uint32_t, uint32_t>> sorted(offs->begin(),
                                                        offs->end());
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
      });
      Json::Value block_offsets(Json::arrayValue);
      for (const auto& [blk, off] : sorted) {
        Json::Value pair(Json::arrayValue);
        pair.append(static_cast<Json::UInt>(blk));
        pair.append(static_cast<Json::UInt>(off));
        block_offsets.append(pair);
      }
      if (!block_offsets.empty()) {
        r["block_offsets"] = block_offsets;
      }
    }
    if (!rec.pgo.empty()) {
      Json::Value pgo(Json::objectValue);
      for (const auto& p : rec.pgo) {
        Json::Value jp;
        jp["appear_pct"] = p.appear_percent;
        jp["call_count"] = p.call_count;
        jp["order_pct"] = p.order_percent;
        pgo[p.interaction] = jp;
      }
      r["pgo"] = pgo;
    }
    if (rec.betamap_rank >= 0) {
      r["betamap_rank"] = static_cast<Json::Int64>(rec.betamap_rank);
    }
    if (rec.interdex_group >= 0) {
      r["interdex_group"] = static_cast<Json::Int64>(rec.interdex_group);
    }
    if (rec.baseline_hot || rec.baseline_startup || rec.baseline_post_startup) {
      Json::Value bp;
      bp["hot"] = rec.baseline_hot;
      bp["startup"] = rec.baseline_startup;
      bp["post_startup"] = rec.baseline_post_startup;
      r["baseline"] = bp;
    }
    if (!rec.callers.empty()) {
      // Reference the interned caller set (caller lists are often huge and
      // shared). Empty sets omit the field entirely.
      r["caller_set"] = static_cast<Json::UInt>(caller_set_ids.at(rec.callers));
    }

    // Record this body record's byte offset for the O(1) seek sidecar.
    const uint64_t byte_off = static_cast<uint64_t>(ofs.tellp());
    writer->write(r, &ofs);
    ofs << "\n";
    skeleton.push_back(SkelEntry{
        rec.id, byte_off,
        r.isMember("code_item_size") ? r["code_item_size"].asUInt64() : 0,
        tsv_safe(r["obfuscated_name"].asString()),
        tsv_safe(r["class"].asString()), tsv_safe(r["method"].asString()),
        'm'});
  }

  // Field records (kind:"field"): fields have no code, so no
  // size/disasm/blocks/ pgo/callers. Emitted after methods (their ids follow
  // the method id space) so the skeleton stays id-ascending.
  for (const FieldRecord& fr : m_fields) {
    Json::Value r;
    r["id"] = static_cast<Json::UInt>(fr.id);
    r["kind"] = "field";
    r["obfuscated_name"] = show(fr.field);
    if (auto* cls = type_class(fr.field->get_class()); cls != nullptr) {
      r["class"] = cls->get_deobfuscated_name_or_empty_copy();
    } else {
      r["class"] = show(fr.field->get_class());
    }
    r["field"] = bare_method(fr.field->get_deobfuscated_name_or_empty());
    r["access"] = static_cast<Json::UInt>(fr.field->get_access());
    if (!fr.dex.empty()) {
      r["dex"] = fr.dex;
    }
    const uint64_t byte_off = static_cast<uint64_t>(ofs.tellp());
    writer->write(r, &ofs);
    ofs << "\n";
    skeleton.push_back(SkelEntry{
        fr.id, byte_off, 0, tsv_safe(r["obfuscated_name"].asString()),
        tsv_safe(r["class"].asString()), tsv_safe(r["field"].asString()), 'f'});
  }

  // Class records (kind:"class"): inheritance edges only (super + interfaces),
  // no code/size, so they are not in the skeleton. Empty super/interfaces are
  // omitted; the read side derives subclasses/implementors by inverting these.
  for (const ClassRecord& cr : m_classes) {
    Json::Value r;
    r["kind"] = "class";
    r["class"] = cr.name;
    if (!cr.super.empty()) {
      r["super"] = cr.super;
    }
    if (!cr.interfaces.empty()) {
      Json::Value intfs(Json::arrayValue);
      for (const auto& i : cr.interfaces) {
        intfs.append(i);
      }
      r["interfaces"] = intfs;
    }
    writer->write(r, &ofs);
    ofs << "\n";
  }

  // A line-addressable skeleton (one method/line; the 1-based line
  // number is the ctags address -- ctags cannot address by byte offset), a
  // byte-offset sidecar (id -> NDJSON offset + skeleton line) for the mmap/seek
  // path, and a universal-ctags file targeting the skeleton.
  const std::string skeleton_name = "redex-dexvt.skeleton.tsv";
  {
    std::ofstream skel(conf.metafile(skeleton_name));
    std::ofstream side(conf.metafile("redex-dexvt.sidecar.tsv"));
    std::ofstream tags(conf.metafile("redex-dexvt.tags"));
    always_assert_log(
        skel.is_open() && side.is_open() && tags.is_open(),
        "dexvt: failed to open skeleton/sidecar/tags for writing");
    tags << "!_TAG_FILE_FORMAT\t2\t/extended format/\n";
    tags << "!_TAG_FILE_SORTED\t0\t/0=unsorted,1=sorted/\n";
    for (size_t i = 0; i < skeleton.size(); ++i) {
      const auto& e = skeleton[i];
      const uint32_t skel_line = static_cast<uint32_t>(i + 1);
      skel << e.id << '\t' << e.obf << '\t' << e.deobf_class << '\t'
           << e.deobf_member << '\t' << e.code_item_size << '\n';
      side << e.id << '\t' << e.byte_off << '\t' << skel_line << '\n';
      // Every entity gets a ctags entry (fall back to the obfuscated name when
      // there is no deobfuscated name -- synthetic/unrenamed), so the tag count
      // equals the record count and the reader can navigate to all of them.
      // tsv_safe already stripped any TAB. kind: 'm' method, 'f' field.
      const std::string& tag_name =
          e.deobf_member.empty() ? e.obf : e.deobf_member;
      tags << tag_name << '\t' << skeleton_name << '\t' << skel_line
           << ";\"\tkind:" << e.kind << "\tclass:" << e.deobf_class << '\n';
    }
  }

  // Caller-set sidecar: one line per distinct set, `set_id<TAB>comma,ids` in
  // artifact (sorted+unique) order. Uncompressed at rest so the pure-stdlib
  // reader can line-seek it. Empty sets never consume a set_id (methods omit
  // `caller_set`).
  {
    std::ofstream cs(conf.metafile("redex-dexvt.callersets.tsv"));
    always_assert_log(cs.is_open(),
                      "dexvt: failed to open callersets for writing");
    for (size_t sid = 0; sid < sets_by_id.size(); ++sid) {
      cs << sid << '\t';
      const auto& ids = *sets_by_id[sid];
      for (size_t j = 0; j < ids.size(); ++j) {
        if (j != 0) {
          cs << ',';
        }
        cs << ids[j];
      }
      cs << '\n';
    }
  }

  TRACE(
      MAIN,
      1,
      "[dexvt] wrote %s (%zu methods, %zu fields, %zu caller-sets, %zu method "
      "sizes)",
      path.c_str(),
      m_records.size(),
      m_fields.size(),
      sets_by_id.size(),
      output_totals.method_size.size());

  // Drop the (whole-app-sized) recorded offsets now they are emitted, and
  // disable the sink so any later IRCode::sync in the process is a no-op.
  block_offset_sink::clear();
}

} // namespace dexvt
