/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IODIMetadata.h"

#include <fstream>

#include "DexOutput.h"
#include "DexUtil.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"

namespace {
// Returns com.foo.Bar. for the DexClass Lcom/foo/Bar;. Note the trailing
// '.'.
std::string pretty_prefix_for_cls(const DexClass* cls) {
  std::string pretty_name = java_names::internal_to_external(cls->str());
  // Include the . separator
  pretty_name.push_back('.');
  return pretty_name;
}
} // namespace

std::string IODIMetadata::get_iodi_name(const DexMethod* m) {
  std::string prefix = pretty_prefix_for_cls(type_class(m->get_class()));
  prefix += m->str();
  return prefix;
}

const std::string& IODIMetadata::get_layered_name(const std::string& base_name,
                                                  size_t layer,
                                                  std::string& storage) {
  if (layer == 0) {
    return base_name;
  }
  storage = base_name;
  storage += "@";
  storage += std::to_string(layer);
  return storage;
}

void IODIMetadata::mark_methods(DexStoresVector& scope) {
  // Calculates which methods won't collide with other methods when printed
  // in a stack trace (e.g. due to method overloading or templating).
  // Before IODI we disambiguated stack trace lines by using a proguard mapping
  // file (which relied on the line numbers in the stack trace to pick
  // which specific method should be symbolicated). Now, if we use instruction
  // offsets in stack traces, then we cannot leverage proguard mappings anymore,
  // so we must disable IODI for any methods whose stack trace may be ambiguous.
  //
  // We do this linearly for now because otherwise we need locks

  std::unordered_map<std::string, DexMethod*> name_method_map;

  std::unordered_map<std::string, const DexMethod*> name_map;
  auto emplace_entry = [&](const std::string& str, DexMethod* m) {
    {
      const DexMethod* canonical;
      auto it = name_map.find(str);
      if (it == name_map.end()) {
        canonical = m;
        name_map.emplace(str, m);
      } else {
        canonical = m_canonical.at(it->second);
      }
      m_canonical[m] = canonical;
      m_name_clusters[canonical].insert(m);
    }

    auto iter = name_method_map.find(str);
    if (iter != name_method_map.end()) {
      auto name_iter = m_method_to_name.find(iter->second);
      if (name_iter != m_method_to_name.end()) {
        m_method_to_name.erase(name_iter);
      }
      iter->second = nullptr;
    } else {
      name_method_map.emplace(str, m);
      m_method_to_name.emplace(m, str);
    }
  };
  for (auto& store : scope) {
    for (auto& classes : store.get_dexen()) {
      for (auto& cls : classes) {
        auto pretty_prefix = pretty_prefix_for_cls(cls);
        // First we need to mark all entries...
        for (DexMethod* m : cls->get_dmethods()) {
          emplace_entry(pretty_prefix + m->str(), m);
        }
        for (DexMethod* m : cls->get_vmethods()) {
          emplace_entry(pretty_prefix + m->str(), m);
        }
      }
    }
  }

  m_marked = true;
}

void IODIMetadata::set_iodi_layer(const DexMethod* method, size_t layer) {
  m_iodi_method_layers.emplace(method,
                               std::make_pair(get_iodi_name(method), layer));
}

size_t IODIMetadata::get_iodi_layer(const DexMethod* method) const {
  auto it = m_iodi_method_layers.find(method);
  return it != m_iodi_method_layers.end() ? it->second.second : 0u;
}

bool IODIMetadata::has_iodi_layer(const DexMethod* method) const {
  auto it = m_iodi_method_layers.find(method);
  return it != m_iodi_method_layers.end();
}

void IODIMetadata::mark_method_huge(const DexMethod* method) {
  m_huge_methods.insert(method);
}

void IODIMetadata::write(
    const std::string& iodi_metadata_filename,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id) {
  if (iodi_metadata_filename.empty()) {
    return;
  }
  std::ofstream ofs(iodi_metadata_filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  write(ofs, method_to_id);
}

void IODIMetadata::write(
    std::ostream& ofs,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id) {
  /*
   * Binary file format
   * {
   *  magic: uint32_t = 0xfaceb001
   *  version: uint32_t = 1
   *  count: uint32_t
   *  zero: uint32_t = 0
   *  single_entries: entry_t[count]
   * }
   * where
   * entry_t = {
   *  klen: uint16_t
   *  method_id: uint64_t
   *  key: char[klen]
   * }
   */
  struct __attribute__((__packed__)) Header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t zero;
  } header = {
      .magic = 0xfaceb001,
      .version = 1,
      .count = 0,
      .zero = 0,
  };
  ofs.write((const char*)&header, sizeof(Header));

  struct __attribute__((__packed__)) EntryHeader {
    uint16_t klen;
    uint64_t method_id;
  } entry_hdr;

  uint32_t count = 0;
  size_t max_layer{0};
  size_t layered_count{0};

  for (const auto& p : m_iodi_method_layers) {
    count += 1;
    always_assert_log(count != 0, "Too many entries found, overflowed");

    auto* method = p.first;
    const auto& name = p.second.first;
    auto layer = p.second.second;
    redex_assert(layer < DexOutput::kIODILayerBound);

    std::string tmp;
    const std::string& layered_name = get_layered_name(name, layer, tmp);

    always_assert(layered_name.size() < UINT16_MAX);
    entry_hdr.klen = layered_name.size();
    entry_hdr.method_id = method_to_id.at(const_cast<DexMethod*>(method));
    ofs.write((const char*)&entry_hdr, sizeof(EntryHeader));
    ofs << layered_name;
  }
  // Rewind and write the header now that we know single/dup counts
  ofs.seekp(0);
  header.count = count;
  ofs.write((const char*)&header, sizeof(Header));
  TRACE(IODI, 1,
        "[IODI] Emitted %u entries, %zu in layers (maximum layer %zu).", count,
        layered_count, max_layer + 1);
}
