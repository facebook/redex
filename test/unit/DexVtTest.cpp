/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexVt.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <json/reader.h>
#include <json/value.h>
#include <map>
#include <sstream>

#include "BlockOffsetSink.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"

namespace {

DexClass* create_foo_class() {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(type::java_lang_Object());
  cc.add_method(assembler::method_from_string(R"(
    (method (public) "LFoo;.bar:()I"
      (
        (const v0 42)
        (return v0)
      )
    )
  )"));
  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.baz:()V"
      (
        (return-void)
      )
    )
  )"));
  cc.add_field(DexField::make_field("LFoo;.counter:I")
                   ->make_concrete(ACC_PRIVATE | ACC_STATIC));
  return cc.create();
}

DexStoresVector make_stores(DexClass* cls) {
  DexStoresVector stores;
  DexStore store("classes");
  store.add_classes({cls});
  stores.emplace_back(std::move(store));
  return stores;
}

ConfigFiles make_config() {
  std::istringstream ss(R"({"redex":{"passes":[]}})");
  Json::Value cfg;
  ss >> cfg;
  return ConfigFiles(cfg);
}

// Two static methods where `caller` invokes `callee`, so the override-resolved
// call graph records a real caller edge (exercises caller-set
// capture/interning).
DexClass* create_caller_class() {
  ClassCreator cc(DexType::make_type("LCallers;"));
  cc.set_super(type::java_lang_Object());
  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LCallers;.callee:()V"
      (
        (return-void)
      )
    )
  )"));
  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LCallers;.caller:()V"
      (
        (invoke-static () "LCallers;.callee:()V")
        (return-void)
      )
    )
  )"));
  DexClass* cls = cc.create();
  // Mark `caller` a root so the complete call graph includes it (and hence the
  // caller->callee edge); synthetic test methods are otherwise unreachable,
  // which would leave the graph -- and every record's callers -- empty.
  for (auto* m : cls->get_dmethods()) {
    if (show(m).find("caller") != std::string::npos) {
      m->rstate.set_root();
    }
  }
  return cls;
}

// A class whose method carries SourceBlocks across a branch (entry /
// fallthrough / left / join), so capture records per-block anchors and, after
// lower+sync, emit can place block_offsets.
DexClass* create_sb_class() {
  ClassCreator cc(DexType::make_type("LSb;"));
  cc.set_super(type::java_lang_Object());
  cc.add_method(assembler::method_from_string(R"(
    (method (public static) "LSb;.m:()V"
      (
        (.src_block "LSb;.m:()V" 1 (1.0 1.0))
        (const v0 0)
        (if-eqz v0 :left)
        (.src_block "LSb;.m:()V" 3 (1.0 1.0))
        (const v1 1)
        (goto :end)
        (:left)
        (.src_block "LSb;.m:()V" 2 (1.0 1.0))
        (const v1 2)
        (:end)
        (.src_block "LSb;.m:()V" 4 (1.0 1.0))
        (return-void)
      )
    )
  )"));
  // An INSTANCE method too: a non-static method's leading `this` load-param is
  // erase_and_dispose()d by instruction_lowering, so a block anchored on it has
  // no anchor left at sync. Params occupy the highest registers of the frame.
  //
  // The load-param MUST come before the SourceBlock, as it does in real code:
  // check_load_params() reads get_param_instructions(), the PREFIX of
  // fallthrough/load-param entries. A leading MFLOW_SOURCE_BLOCK empties that
  // prefix, so check_load_params returns early without erasing, the `this`
  // load-param is merely retyped in place by remove_opcode, its anchor survives
  // -- and this test is disarmed.
  cc.add_method(assembler::method_from_string(R"(
    (method (public) "LSb;.i:()V"
      (
        (load-param-object v2)
        (.src_block "LSb;.i:()V" 1 (1.0 1.0))
        (const v0 0)
        (if-eqz v0 :ileft)
        (.src_block "LSb;.i:()V" 3 (1.0 1.0))
        (const v1 1)
        (goto :iend)
        (:ileft)
        (.src_block "LSb;.i:()V" 2 (1.0 1.0))
        (const v1 2)
        (:iend)
        (.src_block "LSb;.i:()V" 4 (1.0 1.0))
        (return-void)
      )
    )
  )"));
  return cc.create();
}

const DexMethod* find_method(DexClass* cls, const std::string& needle) {
  for (auto* m : cls->get_dmethods()) {
    if (show(m).find(needle) != std::string::npos) {
      return m;
    }
  }
  for (auto* m : cls->get_vmethods()) {
    if (show(m).find(needle) != std::string::npos) {
      return m;
    }
  }
  return nullptr;
}

} // namespace

class DexVtTest : public RedexTest {};

// The pre-lowering capture produces a record with non-empty disassembly for
// every concrete method (P0.M3).
TEST_F(DexVtTest, capturesRecordsWithDisasm) {
  auto* cls = create_foo_class();
  auto stores = make_stores(cls);
  auto conf = make_config();

  dexvt::Exporter exporter;
  exporter.capture_pre_lowering(stores, conf);

  EXPECT_EQ(exporter.num_captured(), 2u);

  UnorderedSet<uint32_t> ids;
  for (auto* m : cls->get_vmethods()) {
    const auto* rec = exporter.get_record(m);
    ASSERT_NE(rec, nullptr);
    EXPECT_FALSE(rec->disasm.empty());
    ids.insert(rec->id);
  }
  for (auto* m : cls->get_dmethods()) {
    const auto* rec = exporter.get_record(m);
    ASSERT_NE(rec, nullptr);
    EXPECT_FALSE(rec->disasm.empty());
    ids.insert(rec->id);
  }
  // Ids are unique across the captured methods.
  EXPECT_EQ(ids.size(), 2u);
}

// The disassembly must be reproducible: byte-identical across independent
// captures and free of the live MethodItemEntry heap pointer that redex's
// show(cfg) embeds on every line ("[0x..]"), which would make the artifact
// non-deterministic and non-diffable. Guards render_disasm (P0).
TEST_F(DexVtTest, disasmIsDeterministicAndPointerFree) {
  auto* cls = create_foo_class();
  auto stores = make_stores(cls);

  auto conf1 = make_config();
  dexvt::Exporter e1;
  e1.capture_pre_lowering(stores, conf1);

  auto conf2 = make_config();
  dexvt::Exporter e2;
  e2.capture_pre_lowering(stores, conf2);

  for (auto* m : cls->get_vmethods()) {
    const auto* r1 = e1.get_record(m);
    const auto* r2 = e2.get_record(m);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r1->disasm, r2->disasm); // reproducible across runs
    EXPECT_EQ(r1->disasm.find("0x"), std::string::npos); // no heap pointers
    EXPECT_NE(r1->disasm.find("B0"), std::string::npos); // block-structured
  }
}

// Fields are captured as their own records, in the id space after methods.
// create_foo_class has one static field.
TEST_F(DexVtTest, capturesFields) {
  auto* cls = create_foo_class();
  auto stores = make_stores(cls);
  auto conf = make_config();

  dexvt::Exporter exporter;
  exporter.capture_pre_lowering(stores, conf);

  EXPECT_EQ(exporter.num_captured(), 2u); // methods unaffected by the field
  EXPECT_EQ(exporter.num_fields(), 1u);
}

// enhanced_dex_stats_t::operator+= must merge method_size (not only
// class_size), otherwise the accumulated output_totals.method_size stays empty
// and the dexvt size join produces nothing. Guards the load-bearing merge.
TEST_F(DexVtTest, mergesMethodSizesInStats) {
  auto* cls = create_foo_class();
  auto vmethods = cls->get_vmethods();
  auto dmethods = cls->get_dmethods();
  ASSERT_FALSE(vmethods.empty());
  ASSERT_FALSE(dmethods.empty());

  enhanced_dex_stats_t a;
  a.method_size[vmethods[0]] = 10;
  enhanced_dex_stats_t b;
  b.method_size[dmethods[0]] = 20;

  a += b;

  EXPECT_EQ(a.method_size.size(), 2u);
  EXPECT_EQ(a.method_size.at(vmethods[0]), 10u);
  EXPECT_EQ(a.method_size.at(dmethods[0]), 20u);
}

// The override-resolved call graph populates MethodRecord::callers -- the data
// that feeds caller-set interning. `caller` must appear among `callee`'s
// callers.
TEST_F(DexVtTest, capturesCallGraphCallers) {
  auto* cls = create_caller_class();
  auto stores = make_stores(cls);
  auto conf = make_config();

  dexvt::Exporter exporter;
  exporter.capture_pre_lowering(stores, conf);

  const DexMethod* callee = find_method(cls, "callee");
  const DexMethod* caller = find_method(cls, "caller");
  ASSERT_NE(callee, nullptr);
  ASSERT_NE(caller, nullptr);
  const auto* callee_rec = exporter.get_record(callee);
  const auto* caller_rec = exporter.get_record(caller);
  ASSERT_NE(callee_rec, nullptr);
  ASSERT_NE(caller_rec, nullptr);
  EXPECT_NE(std::find(callee_rec->callers.begin(), callee_rec->callers.end(),
                      caller_rec->id),
            callee_rec->callers.end());
}

// emit() writes schema_version 8, references caller-sets by a `caller_set` int,
// resolves each set through the redex-dexvt.callersets.tsv sidecar, emits
// kind:"class" inheritance records (v5), and per-method block_offsets for every
// block including the entry block (v8).
TEST_F(DexVtTest, emitInternsCallerSets) {
  auto* cls = create_caller_class();
  auto stores = make_stores(cls);
  auto conf = make_config();

  namespace fs = std::filesystem;
  auto tmp = redex::make_tmp_dir("dexvt_emit_intern%%%%%%%%");
  fs::path dir(tmp.path);
  conf.set_outdir(tmp.path);

  dexvt::Exporter exporter;
  exporter.capture_pre_lowering(stores, conf);
  enhanced_dex_stats_t totals;
  exporter.emit(conf, totals);

  std::ifstream nd((dir / "meta" / "redex-dexvt.ndjson").string());
  ASSERT_TRUE(nd.is_open());
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(nd, line)));
  Json::Value manifest;
  ASSERT_TRUE(Json::Reader().parse(line, manifest));
  EXPECT_EQ(manifest["schema_version"].asInt(), 8);
  EXPECT_GE(manifest["callerset_count"].asUInt(), 1u);
  EXPECT_GE(manifest["class_count"].asUInt(), 1u);

  int caller_set = -1;
  bool saw_class = false;
  while (std::getline(nd, line)) {
    Json::Value r;
    ASSERT_TRUE(Json::Reader().parse(line, r));
    EXPECT_FALSE(r.isMember("callers")); // callers are interned, not inlined
    if (r.isMember("caller_set")) {
      caller_set = r["caller_set"].asInt();
    }
    if (r.isMember("kind") && r["kind"].asString() == "class") {
      saw_class = true; // v5 emits kind:"class" inheritance records
    }
  }
  ASSERT_GE(caller_set, 0);
  EXPECT_TRUE(saw_class);

  std::ifstream cs((dir / "meta" / "redex-dexvt.callersets.tsv").string());
  ASSERT_TRUE(cs.is_open());
  bool found = false;
  while (std::getline(cs, line)) {
    auto tab = line.find('\t');
    ASSERT_NE(tab, std::string::npos);
    if (std::stoi(line.substr(0, tab)) == caller_set) {
      EXPECT_FALSE(line.substr(tab + 1).empty()); // >=1 caller id
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// Dex strings are MUTF-8: a supplementary char (here U+1F9D9) is a surrogate
// pair (CESU-8), which is invalid UTF-8 -- the raw bytes would be mangled to
// U+FFFD by the NDJSON writer. render_disasm must decode CONST_STRING operands
// to proper UTF-8.
TEST_F(DexVtTest, disasmDecodesMutf8Strings) {
  const std::string mutf8 = "\xED\xA0\xBE\xED\xB7\x99"; // CESU-8 of U+1F9D9
  const std::string utf8 = "\xF0\x9F\xA7\x99"; // proper UTF-8 of U+1F9D9

  ClassCreator cc(DexType::make_type("LEmoji;"));
  cc.set_super(type::java_lang_Object());
  cc.add_method(assembler::method_from_string(
      "(method (public static) \"LEmoji;.s:()V\" ("
      "(const-string \"" +
      mutf8 +
      "\") "
      "(move-result-pseudo-object v0) "
      "(return-void)))"));
  auto* cls = cc.create();
  auto stores = make_stores(cls);
  auto conf = make_config();

  dexvt::Exporter exporter;
  exporter.capture_pre_lowering(stores, conf);

  const DexMethod* m = find_method(cls, "LEmoji;.s");
  ASSERT_NE(m, nullptr);
  const auto* rec = exporter.get_record(m);
  ASSERT_NE(rec, nullptr);
  EXPECT_NE(rec->disasm.find(utf8), std::string::npos); // decoded to real UTF-8
  EXPECT_EQ(rec->disasm.find("\xED\xA0"),
            std::string::npos); // no raw surrogate bytes
  EXPECT_EQ(rec->disasm.find("\xEF\xBF\xBD"), std::string::npos); // no U+FFFD
}

// emit() places per-block shipped-DEX offsets by resolving each block's
// pre-lowering first-MethodItemEntry anchor against the code-unit offsets the
// BlockOffsetSink records during IRCode::sync -- for EVERY block, not only
// source-blocked ones. Drives the real capture -> lower -> sync -> emit order
// in-process (a plain capture+emit never syncs, so the sink stays empty),
// exercising the anchor's survival across lowering.
TEST_F(DexVtTest, emitsBlockOffsets) {
  block_offset_sink::clear();
  auto* cls = create_sb_class();
  auto stores = make_stores(cls);
  auto conf = make_config();

  namespace fs = std::filesystem;
  auto tmp = redex::make_tmp_dir("dexvt_block_offsets%%%%%%%%");
  fs::path dir(tmp.path);
  conf.set_outdir(tmp.path);

  dexvt::Exporter exporter;
  exporter.capture_pre_lowering(stores,
                                conf); // records anchors, enables the sink
  instruction_lowering::run(stores); // IR -> dex opcodes (SourceBlocks survive)
  // Drive sync so the sink records each SourceBlock's final code-unit offset
  // (the real backend does this inside DexOutput::generate_code_items).
  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() != nullptr) {
      m->get_code()->sync(m);
    }
  }
  enhanced_dex_stats_t totals;
  exporter.emit(conf, totals);

  std::ifstream nd((dir / "meta" / "redex-dexvt.ndjson").string());
  ASSERT_TRUE(nd.is_open());
  std::string line;
  Json::Value method_rec;
  bool found = false;
  bool saw_instance_method = false;
  std::map<std::string, unsigned> min_cu_by_method;
  while (std::getline(nd, line)) {
    Json::Value r;
    ASSERT_TRUE(Json::Reader().parse(line, r));
    if (r["kind"] == "method" && r.isMember("block_offsets")) {
      method_rec = r;
      found = true;
      const std::string name = r["obfuscated_name"].asString();
      if (name.find("LSb;.i:") != std::string::npos) {
        saw_instance_method = true;
      }
      unsigned lo = UINT_MAX;
      for (const auto& pair : r["block_offsets"]) {
        lo = std::min(lo, pair[1u].asUInt());
      }
      min_cu_by_method[name] = lo;
    }
  }
  ASSERT_TRUE(found);

  const Json::Value& bo = method_rec["block_offsets"];
  ASSERT_TRUE(bo.isArray());
  ASSERT_GT(bo.size(), 0u);
  std::vector<unsigned> off_blks;
  unsigned prev_cu = 0;
  for (const auto& pair : bo) {
    ASSERT_EQ(pair.size(), 2u); // [blk, start_cu]
    off_blks.push_back(pair[0u].asUInt());
    // Emitted ascending by (start_cu, blk) to match the read side.
    EXPECT_GE(pair[1u].asUInt(), prev_cu);
    prev_cu = pair[1u].asUInt();
  }
  // Every block that carries hotness must be addressable via block_offsets --
  // and, since v8 anchors every block, so must non-source-blocked blocks, so
  // the offset set is a superset of the (source-blocked) hot set.
  ASSERT_TRUE(method_rec["blocks"].isMember("blk"));
  for (const auto& b : method_rec["blocks"]["blk"]) {
    EXPECT_NE(std::find(off_blks.begin(), off_blks.end(), b.asUInt()),
              off_blks.end())
        << "hot block " << b.asUInt() << " missing from block_offsets";
  }

  // The ENTRY block must be anchored at code-unit 0 for EVERY method that has
  // offsets at all -- static and instance alike. This DOES reproduce the
  // entry-block loss end to end: revert the load-param skip in DexVt.cpp and
  // `LSb;.i:()V` loses its blk-0 row.
  EXPECT_TRUE(saw_instance_method)
      << "fixture must cover an instance method (the regressing case)";
  for (const auto& [name, min_cu] : min_cu_by_method) {
    EXPECT_EQ(min_cu, 0u) << name << ": lowest block_offsets start_cu is "
                          << min_cu << ", so the entry block has no offset";
  }
  block_offset_sink::clear();
}
