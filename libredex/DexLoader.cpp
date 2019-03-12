/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/iostreams/device/mapped_file.hpp>

#include "DexLoader.h"
#include "DexDefs.h"
#include "DexAccess.h"
#include "IRCode.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

#include <exception>
#include <stdexcept>
#include <vector>

class DexLoader {
  DexIdx* m_idx;
  const dex_class_def* m_class_defs;
  DexClasses* m_classes;
  boost::iostreams::mapped_file m_file;
  std::string m_dex_location;

 public:
  explicit DexLoader(const char* location)
      : m_idx(nullptr), m_dex_location(location) {}
  ~DexLoader() {
    if (m_idx) delete m_idx;
    if (m_file.is_open()) m_file.close();
  }
  const dex_header* get_dex_header(const char* location);
  DexClasses load_dex(const char* location,
                      dex_stats_t* stats,
                      bool support_dex_v37);
  void load_dex_class(int num);
  void gather_input_stats(dex_stats_t* stats, const dex_header* dh);
};

static void validate_dex_header(const dex_header* dh,
                                size_t dexsize,
                                bool support_dex_v37) {
  if (support_dex_v37) {
    // support_dex_v37 flag enables parsing v37 dex, but does not disable
    // parsing v35 dex.
    if (memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V37, sizeof(dh->magic)) &&
        memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V35, sizeof(dh->magic))) {
      always_assert_log(false, "Bad v35 or v37 dex magic %s\n", dh->magic);
    }
  } else {
    if (memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V35, sizeof(dh->magic))) {
      always_assert_log(false, "Bad v35 dex magic %s\n", dh->magic);
    }
  }
  always_assert_log(
    dh->file_size == dexsize,
    "Reported size in header (%z) does not match file size (%u)\n",
    dexsize,
    dh->file_size);
}

struct class_load_work {
  DexLoader* dl;
  int num;
};

static std::vector<std::exception_ptr> class_work(class_load_work* clw) {
  try {
    clw->dl->load_dex_class(clw->num);
    return {}; // no exception
  } catch (const std::exception& exc) {
    TRACE(MAIN, 1, "Worker throw the exception:%s\n", exc.what());

    return {std::current_exception()};
  }
}

static std::vector<std::exception_ptr> exc_reducer(
    const std::vector<std::exception_ptr>& v1,
    const std::vector<std::exception_ptr>& v2) {
  if (v1.empty()) {
    return v2;
  } else if (v2.empty()) {
    return v1;
  } else {
    std::vector<std::exception_ptr> result (v1);
    result.insert(result.end(), v2.begin(), v2.end());
    return result;
  }
}

void DexLoader::gather_input_stats(dex_stats_t* stats, const dex_header* dh) {
  stats->num_types += dh->type_ids_size;
  stats->num_classes += dh->class_defs_size;
  stats->num_method_refs += dh->method_ids_size;
  stats->num_field_refs += dh->field_ids_size;
  stats->num_strings += dh->string_ids_size;
  stats->num_protos += dh->proto_ids_size;
  stats->num_bytes += dh->file_size;

  std::unordered_set<DexEncodedValueArray,
                     boost::hash<DexEncodedValueArray>>
    enc_arrays;
  std::set<DexTypeList*, dextypelists_comparator> type_lists;
  std::unordered_set<uint32_t> anno_offsets;
  for (uint32_t cidx = 0; cidx < dh->class_defs_size; ++cidx) {
    auto* clz = m_classes->at(cidx);
    auto* class_def = &m_class_defs[cidx];
    auto anno_off = class_def->annotations_off;
    if (anno_off) {
      const dex_annotations_directory_item* anno_dir =
        (const dex_annotations_directory_item*)m_idx->get_uint_data(anno_off);
      auto class_anno_off = anno_dir->class_annotations_off;
      if (class_anno_off) {
        const uint32_t* anno_data = m_idx->get_uint_data(class_anno_off);
        uint32_t count = *anno_data++;
        for (uint32_t aidx = 0; aidx < count; ++aidx) {
          anno_offsets.insert(anno_data[aidx]);
        }
      }
      const uint32_t* anno_data = (uint32_t*)(anno_dir + 1);
      for (uint32_t fidx = 0; fidx < anno_dir->fields_size; ++fidx) {
        anno_data++;
        anno_offsets.insert(*anno_data++);
      }
      for (uint32_t midx = 0; midx < anno_dir->methods_size; ++midx) {
        anno_data++;
        anno_offsets.insert(*anno_data++);
      }
      for (uint32_t pidx = 0; pidx < anno_dir->parameters_size; ++pidx) {
        anno_data++;
        uint32_t xrefoff = *anno_data++;
        if (xrefoff != 0) {
          const uint32_t* annoxref = m_idx->get_uint_data(xrefoff);
          uint32_t count = *annoxref++;
          for (uint32_t j = 0; j < count; j++) {
            uint32_t off = annoxref[j];
            anno_offsets.insert(off);
          }
        }
      }
    }
    auto* interfaces_type_list = clz->get_interfaces();
    type_lists.insert(interfaces_type_list);
    std::unique_ptr<DexEncodedValueArray> deva(clz->get_static_values());
    if (deva) {
      if (!enc_arrays.count(*deva)) {
        enc_arrays.emplace(std::move(*deva.release()));
        stats->num_static_values++;
      }
    }
    stats->num_fields +=
        clz->get_ifields().size() + clz->get_sfields().size();
    stats->num_methods +=
        clz->get_vmethods().size() + clz->get_dmethods().size();
    for (auto* meth : clz->get_vmethods()) {
      DexCode* code = meth->get_dex_code();
      if (code) {
        stats->num_instructions += code->get_instructions().size();
      }
    }
    for (auto* meth : clz->get_dmethods()) {
      DexCode* code = meth->get_dex_code();
      if (code) {
        stats->num_instructions += code->get_instructions().size();
      }
    }
  }
  for (uint32_t meth_idx = 0; meth_idx < dh->method_ids_size; ++meth_idx) {
    auto* meth = m_idx->get_methodidx(meth_idx);
    DexProto* proto = meth->get_proto();
    type_lists.insert(proto->get_args());
  }
  stats->num_annotations += anno_offsets.size();
  stats->num_type_lists += type_lists.size();
}

void DexLoader::load_dex_class(int num) {
  const dex_class_def* cdef = m_class_defs + num;
  DexClass* dc = new DexClass(m_idx, cdef, m_dex_location);
  m_classes->at(num) = dc;
}

const dex_header* DexLoader::get_dex_header(const char* location) {
  m_file.open(location, boost::iostreams::mapped_file::readonly);
  if (!m_file.is_open()) {
    fprintf(stderr, "error: cannot create memory-mapped file: %s\n", location);
    exit(EXIT_FAILURE);
  }
  return reinterpret_cast<const dex_header*>(m_file.const_data());
}

DexClasses DexLoader::load_dex(const char* location,
                               dex_stats_t* stats,
                               bool support_dex_v37) {
  auto dh = get_dex_header(location);
  validate_dex_header(dh, m_file.size(), support_dex_v37);
  if (dh->class_defs_size == 0) {
    return DexClasses(0);
  }
  m_idx = new DexIdx(dh);
  auto off = (uint64_t)dh->class_defs_off;
  auto limit = off + dh->class_defs_size * sizeof(dex_class_def);
  always_assert_log(off < m_file.size(), "class_defs_off out of range");
  always_assert_log(limit <= m_file.size(), "invalid class_defs_size");
  m_class_defs =
      reinterpret_cast<const dex_class_def*>(m_file.const_data() + off);
  DexClasses classes(dh->class_defs_size);
  m_classes = &classes;

  auto lwork = new class_load_work[dh->class_defs_size];
  auto wq =
      workqueue_mapreduce<class_load_work*, std::vector<std::exception_ptr>>(
        class_work, exc_reducer);
  for (uint32_t i = 0; i < dh->class_defs_size; i++) {
    lwork[i].dl = this;
    lwork[i].num = i;
    wq.add_item(&lwork[i]);
  }
  const auto exceptions = wq.run_all();
  delete[] lwork;

  if (!exceptions.empty()) {
    // At least one of the workers raised an exception
    aggregate_exception ae(exceptions);
    throw ae;
  }

  gather_input_stats(stats, dh);

  return classes;
}

static void mt_balloon(DexMethod* method) { method->balloon(); }

static void balloon_all(const Scope& scope) {
  auto wq = workqueue_foreach<DexMethod*>(mt_balloon);
  walk::methods(scope, [&](DexMethod* m) {
    if (m->get_dex_code()) {
      wq.add_item(m);
    }
  });
  wq.run_all();
}

DexClasses load_classes_from_dex(const char* location,
                                 bool balloon,
                                 bool support_dex_v37) {
  dex_stats_t stats;
  return load_classes_from_dex(location, &stats, balloon, support_dex_v37);
}

DexClasses load_classes_from_dex(const char* location,
                                 dex_stats_t* stats,
                                 bool balloon,
                                 bool support_dex_v37) {
  TRACE(MAIN, 1, "Loading classes from dex from %s\n", location);
  DexLoader dl(location);
  auto classes = dl.load_dex(location, stats, support_dex_v37);
  if (balloon) {
    balloon_all(classes);
  }
  return classes;
}

const std::string load_dex_magic_from_dex(const char* location) {
  DexLoader dl(location);
  auto dh = dl.get_dex_header(location);
  return dh->magic;
}

void balloon_for_test(const Scope& scope) { balloon_all(scope); }
