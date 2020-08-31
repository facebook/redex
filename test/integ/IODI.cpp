/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "DexDefs.h"
#include "DexEncoding.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "IODIMetadata.h"
#include "InstructionLowering.h"
#include "RedexContext.h"
#include "SanitizersConfig.h"
#include "Show.h"
#include "Walkers.h"

struct DexOutputTestHelper {
  static uint8_t* steal_output(DexOutput& output) {
    return std::exchange(output.m_output, nullptr);
  }
};

namespace {

void reset_redex() {
  if (g_redex) {
    delete g_redex;
  }
  g_redex = new RedexContext();
}

DexClasses run_redex(std::unordered_map<std::string, uint64_t>* mid = nullptr,
                     std::string* iodi_data = nullptr) {
  reset_redex();
  const char* dexfile = std::getenv("dexfile");
  EXPECT_NE(nullptr, dexfile);
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile));
  stores.emplace_back(std::move(root_store));

  instruction_lowering::run(stores, true);

  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make("tmp"));
  std::unordered_map<DexMethod*, uint64_t> method_to_id;
  std::unordered_map<DexCode*, std::vector<DebugLineItem>> code_debug_lines;
  IODIMetadata iodi_metadata;
  iodi_metadata.mark_methods(stores);

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_cfg(conf_obj);

  always_assert(stores.size() == 1);
  auto& store = stores[0];
  auto& dexen = store.get_dexen();
  auto store_name = store.get_name();
  always_assert(dexen.size() == 1);
  DexOutput output("tmp.dex", /* filename */
                   dexen.data(),
                   nullptr, /* locator_index */
                   false, /* normal_primary_dex */
                   0,
                   0,
                   DebugInfoKind::InstructionOffsets,
                   &iodi_metadata,
                   dummy_cfg,
                   pos_mapper.get(),
                   &method_to_id,
                   &code_debug_lines);
  output.prepare(
      SortMode::DEFAULT, {SortMode::DEFAULT}, dummy_cfg, "dex\n035\0");
  if (mid) {
    for (auto& iter : method_to_id) {
      DexMethod* method = iter.first;
      std::string pretty_name =
          java_names::internal_to_external(method->get_class()->str());
      pretty_name.push_back('.');
      pretty_name += method->str();
      auto mid_iter = mid->find(pretty_name);
      if (mid_iter != mid->end()) {
        mid_iter->second = 0;
      } else {
        auto res = mid->emplace(std::move(pretty_name), iter.second);
        always_assert(res.second);
      }
    }
    for (auto it = mid->begin(); it != mid->end();) {
      if (it->second == 0) {
        it = mid->erase(it);
      } else {
        it++;
      }
    }
  }
  if (iodi_data) {
    std::stringstream sstream;
    iodi_metadata.write(sstream, method_to_id);
    *iodi_data = sstream.str();
  }
  reset_redex();
  uint8_t* data = DexOutputTestHelper::steal_output(output);
  auto result = load_classes_from_dex(
      reinterpret_cast<dex_header*>(data), "tmp.dex", false);
  free(data);
  return result;
}

bool is_iodi(const DexDebugItem& debug_item) {
  if (debug_item.get_line_start() != 0) {
    return false;
  }
  const auto& entries = debug_item.get_entries();
  for (size_t i = 0; i < entries.size(); i++) {
    auto& entry = entries[i];
    if (entry.type != DexDebugEntryType::Position) {
      continue;
    }
    if (i != entry.addr || i != entry.pos->line) {
      return false;
    }
  }
  return true;
}

size_t debug_item_line_table_size(const DexDebugItem& debug_item) {
  size_t result = 0;
  const auto& entries = debug_item.get_entries();
  for (auto& entry : entries) {
    if (entry.type == DexDebugEntryType::Position) {
      result += 1;
    }
  }
  return result;
}

struct DexDebugComp {
  bool operator()(const DexDebugItem* lhs, const DexDebugItem* rhs) const {
    if (lhs->get_source_checksum() != rhs->get_source_checksum()) {
      return lhs->get_source_checksum() < rhs->get_source_checksum();
    } else {
      return lhs->get_source_offset() < rhs->get_source_offset();
    }
  }
};

using DexMethods = std::vector<DexMethod*>;
using DexDebugMap = std::map<DexDebugItem*, DexMethods, DexDebugComp>;
DexDebugMap debug_to_methods(const DexClasses& classes) {
  DexDebugMap result;
  walk::methods(classes, [&](DexMethod* method) {
    DexCode* code = method->get_dex_code();
    if (!code) {
      return;
    }
    DexDebugItem* debug_item = code->get_debug_item();
    if (!debug_item) {
      return;
    }
    result[debug_item].push_back(method);
  });
  return result;
}

#if 0
void log_debug_to_methods(const std::map<void*, DexMethods>& result) {
  for (auto& dtm : result) {
    std::cerr << "{\n";
    const auto& entries = ((DexDebugItem*)dtm.first)->get_entries();
    std::cerr << "  \"positions\" : [\n";
    for (auto& entry : entries) {
      if (entry.type != DexDebugEntryType::Position) {
        continue;
      }
      std::cerr << "    " << entry.addr << " : " << entry.pos->line << ",\n";
    }
    std::cerr << "  ],\n  \"is_iodi\" : "
              << (is_iodi(*(DexDebugItem*)dtm.first) ? "\"true\"" : "\"false\"")
              << ",\n  \"methods\" : [\n";
    for (DexMethod* method : dtm.second) {
      std::cerr << "    \"" << show(method) << "\",\n";
    }
    std::cerr << "  ],\n}\n";
  }
}
#endif

std::unordered_map<std::string, uint32_t> extract_method_to_debug_size() {
  reset_redex();
  std::unordered_map<std::string, uint32_t> result;
  const char* dexfile = std::getenv("dexfile");
  redex_assert(dexfile);
  auto pre_classes = load_classes_from_dex(dexfile, false);
  auto pre_debug_data = debug_to_methods(pre_classes);
  for (auto& data : pre_debug_data) {
    EXPECT_EQ(data.second.size(), 1);
    result.emplace(show(data.second[0]),
                   ((DexDebugItem*)data.first)->get_on_disk_size());
  }
  return result;
}

struct PCCompare {
  bool operator()(const DexMethod* lhs, const DexMethod* rhs) const {
    if (lhs->get_dex_code()->size() == rhs->get_dex_code()->size()) {
      return (uintptr_t)lhs > (uintptr_t)rhs;
    } else {
      return lhs->get_dex_code()->size() > rhs->get_dex_code()->size();
    }
  }
};

} // namespace

TEST(IODITest, avoidDexLayoutOOM) {
  // Here we want to test that for each debug program the inflated size in
  // dexlayout on Android 8, 8.1 is bounded by 8k (arbitrary number, yes,
  // but in practice works out)
  auto classes = run_redex();
  auto debug_data = debug_to_methods(classes);
  for (auto& data : debug_data) {
    DexDebugItem* debug_item = (DexDebugItem*)data.first;
    if (!is_iodi(*debug_item)) {
      continue;
    }
    size_t inflated_size =
        debug_item_line_table_size(*debug_item) * data.second.size();
    EXPECT_LE(inflated_size, 8 * 1024);
  }
}

// Below will need to compute debug program size (meaning including the
// header)
TEST(IODITest, usingIODIWorthIt) {
  auto method_to_pre_debug_size = extract_method_to_debug_size();
  EXPECT_GT(method_to_pre_debug_size.size(), 0);
  auto classes = run_redex();
  auto debug_data = debug_to_methods(classes);
  for (auto& data : debug_data) {
    DexDebugItem* debug_item = (DexDebugItem*)data.first;
    if (!is_iodi(*debug_item)) {
      continue;
    }
    std::set<DexMethod*, PCCompare> methods;
    methods.insert(data.second.begin(), data.second.end());
    EXPECT_EQ(methods.size(), data.second.size());
    EXPECT_GT(methods.size(), 1);

    auto next_biggest = methods.begin();
    uint32_t biggest_insns = (*next_biggest)->get_dex_code()->size();
    while ((*next_biggest)->get_dex_code()->size() == biggest_insns &&
           ++next_biggest != methods.end())
      ;

    uint32_t total_debug_size = 0;
    for (auto iter = methods.begin(); iter != next_biggest; iter++) {
      auto size_iter = method_to_pre_debug_size.find(show(*iter));
      ASSERT_NE(size_iter, method_to_pre_debug_size.end());
      total_debug_size += size_iter->second;
    }

    if (next_biggest != methods.end()) {
      // Check that removing the biggest method results in growing the binary
      EXPECT_GT(total_debug_size + (*next_biggest)->get_dex_code()->size(),
                biggest_insns);
      for (auto iter = next_biggest; iter != methods.end(); iter++) {
        auto size_iter = method_to_pre_debug_size.find(show(*iter));
        ASSERT_NE(size_iter, method_to_pre_debug_size.end());
        total_debug_size += size_iter->second;
      }
      // Make sure it's worth it to create IODI whatsoever
      EXPECT_GT(total_debug_size, (*next_biggest)->get_dex_code()->size());
    }
  }
}

TEST(IODITest, noCrossAritySharing) {
  // In certain lower versions of Android if the arity of a debug item
  // doesn't match the arity of the method pointing to it then symbolication
  // will fail.
  auto classes = run_redex();
  auto debug_data = debug_to_methods(classes);
  for (auto& data : debug_data) {
    DexDebugItem* debug_item = (DexDebugItem*)data.first;
    if (!is_iodi(*debug_item)) {
      continue;
    }
    ASSERT_GT(data.second.size(), 0);
    size_t arity = data.second[0]->get_proto()->get_args()->size();
    for (auto& meth : data.second) {
      EXPECT_EQ(meth->get_proto()->get_args()->size(), arity);
    }
  }
}

TEST(IODITest, iodiBigEnough) {
  auto classes = run_redex();
  auto debug_data = debug_to_methods(classes);
  for (auto& data : debug_data) {
    DexDebugItem* debug_item = (DexDebugItem*)data.first;
    if (!is_iodi(*debug_item)) {
      continue;
    }
    for (DexMethod* method : data.second) {
      ASSERT_GE(debug_item_line_table_size(*debug_item),
                method->get_dex_code()->size());
    }
  }
}

TEST(IODITest, someUseIODI) {
  auto classes = run_redex();
  auto debug_data = debug_to_methods(classes);
  uint32_t iodi_count = 0;
  for (auto& data : debug_data) {
    DexDebugItem* debug_item = (DexDebugItem*)data.first;
    if (!is_iodi(*debug_item)) {
      continue;
    }
    iodi_count += 1;
  }
  EXPECT_GT(iodi_count, 0);
}

TEST(IODITest, sameNameDontUseIODI) {
  auto classes = run_redex();
  size_t same_name_count = 0;
  for (DexClass* cls : classes) {
    std::unordered_map<std::string, DexMethods> name_to_methods;
    for (DexMethod* method : cls->get_dmethods()) {
      name_to_methods[method->str()].push_back(method);
    }
    for (DexMethod* method : cls->get_vmethods()) {
      name_to_methods[method->str()].push_back(method);
    }

    for (auto& iter : name_to_methods) {
      if (iter.second.size() == 1) {
        continue;
      }
      for (DexMethod* method : iter.second) {
        DexCode* code = method->get_dex_code();
        if (!code) {
          continue;
        }
        DexDebugItem* debug_item = code->get_debug_item();
        if (!debug_item) {
          continue;
        }
        EXPECT_FALSE(is_iodi(*debug_item));
        same_name_count += 1;
      }
    }
  }
  // <init>, <init>, sameName, sameName, sameName
  EXPECT_EQ(same_name_count, 5);
}

namespace {
struct IODIParser {
  template <typename T>
  const T* parse(size_t len = sizeof(T)) {
    check_end();
    const T* result = (const T*)cursor;
    cursor += len;
    return result;
  }

  void check_end() {
    if (cursor >= end) {
      throw std::runtime_error("Attempting to parse past end of buffer");
    }
  }

  void ensure_at_end() {
    if (cursor != end) {
      std::string err("Parse error, expected to be at end of buffer. ");
      err += std::to_string(end - cursor) + " unknown bytes";
      throw std::runtime_error(err);
    }
  }

  const uint8_t* cursor;
  const uint8_t* end;
};
} // namespace

TEST(IODITest, encodedMetadataContainsAllIODI) {
  std::string iodi_data;
  std::unordered_map<std::string, uint64_t> mid;
  auto classes = run_redex(&mid, &iodi_data);
  EXPECT_GT(mid.size(), 0);
  EXPECT_GT(iodi_data.size(), 0);

  // First verify all methods with IODI are in the method_id map
  auto debug_data = debug_to_methods(classes);
  std::unordered_map<std::string, uint64_t> iodi_mid;
  for (auto& data : debug_data) {
    DexDebugItem* debug_item = (DexDebugItem*)data.first;
    if (!is_iodi(*debug_item)) {
      continue;
    }
    for (DexMethod* method : data.second) {
      std::string pretty_name =
          java_names::internal_to_external(method->get_class()->str());
      pretty_name.push_back('.');
      pretty_name += method->str();
      auto iter = mid.find(pretty_name);
      EXPECT_NE(iter, mid.end());
      iodi_mid.emplace(pretty_name, iter->second);
    }
  }

  /*
   * Binary file format
   * {
   *  magic: uint32_t = 0xfaceb001
   *  version: uint32_t = 1
   *  count: uint32_t
   *  zero: uint32_t = 0
   *  entries: entry_t[count]
   * }
   * where
   * entry_t = {
   *  klen: uint16_t
   *  method_id: uint64_t
   *  key: char[klen]
   * }
   */
  auto buffer = (uint8_t*)iodi_data.data();
  auto buffer_len = iodi_data.size();
  IODIParser p{buffer, buffer + buffer_len};
  struct __attribute__((__packed__)) Header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t zero;
  };
  // Now verify the resulting metadata file parses correctly
  const Header& hdr = *p.parse<Header>();
  EXPECT_EQ(hdr.magic, 0xfaceb001);
  EXPECT_EQ(hdr.version, 1);
  EXPECT_EQ(hdr.count, iodi_mid.size());
  EXPECT_EQ(hdr.zero, 0);
  struct __attribute__((__packed__)) EntryHeader {
    uint16_t klen;
    uint64_t method_id;
  };
  for (uint32_t i = 0; i < hdr.count; i++) {
    const EntryHeader& entry = *p.parse<EntryHeader>();
    const char* key_c = p.parse<char>(entry.klen);
    EXPECT_EQ(iodi_mid.at(std::string(key_c, entry.klen)), entry.method_id);
  }
  p.ensure_at_end();
}
