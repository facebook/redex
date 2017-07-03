/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "DexLoader.h"
#include "DexDefs.h"
#include "DexAccess.h"
#include "Trace.h"
#include "Transform.h"
#include "Walkers.h"
#include "WorkQueue.h"

#define DL_FAIL (1)
#define DL_SUCCESS (0)

class DexLoader {
  DexIdx* m_idx;
  dex_class_def* m_class_defs;
  DexClasses* m_classes;
  uint8_t* m_dexmmap;
  size_t m_dex_size;
  std::string m_dex_location;

 public:
  explicit DexLoader(const char* location) : m_dex_location(location) {}
  ~DexLoader() {
    if (m_idx) delete m_idx;
    if (m_dexmmap) munmap(m_dexmmap, m_dex_size);
  }
  DexClasses load_dex(const char* location, dex_stats_t* stats);
  void load_dex_class(int num);
  void gather_input_stats(dex_stats_t* stats, dex_header* dh);
};

static int open_dex_file(const char* location,
                         uint8_t*& dmapping,
                         size_t& dsize) {
  int fd = open(location, O_RDONLY);
  struct stat buf;
  if (fd < 0) {
    fprintf(stderr, "Cannot open dump file %s\n", location);
    return DL_FAIL;
  }
  if (fstat(fd, &buf)) {
    fprintf(stderr, "Cannot fstat file %s\n", location);
    return DL_FAIL;
  }
  dsize = buf.st_size;
  dmapping =
      (uint8_t*)mmap(nullptr, dsize, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
  close(fd);
  if (dmapping == MAP_FAILED) {
    dmapping = nullptr;
    perror("Address space allocation failed for mmap\n");
    return DL_FAIL;
  }
  return DL_SUCCESS;
}

static void validate_dex_header(dex_header* dh, size_t dexsize) {
  if (memcmp(dh->magic, DEX_HEADER_DEXMAGIC, sizeof(dh->magic))) {
    always_assert_log(false, "Bad dex magic %s\n", dh->magic);
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

static void class_work(class_load_work* clw) {
  clw->dl->load_dex_class(clw->num);
}

void DexLoader::gather_input_stats(dex_stats_t* stats, dex_header* dh) {
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
  dex_class_def* cdef = m_class_defs + num;
  DexClass* dc = new DexClass(m_idx, cdef, m_dex_location);
  m_classes->at(num) = dc;
}

DexClasses DexLoader::load_dex(const char* location, dex_stats_t* stats) {
  dex_header* dh;
  if (open_dex_file(location, m_dexmmap, m_dex_size) != DL_SUCCESS) {
    exit(1); // FIXME(snay)
  }
  dh = (dex_header*)m_dexmmap;
  validate_dex_header(dh, m_dex_size);
  if (dh->class_defs_size == 0) {
    return DexClasses(0);
  }
  m_idx = new DexIdx(dh);
  auto off = (uint64_t)dh->class_defs_off;
  auto limit = off + dh->class_defs_size * sizeof(dex_class_def);
  always_assert_log(off < m_dex_size, "class_defs_off out of range");
  always_assert_log(limit <= m_dex_size, "invalid class_defs_size");
  m_class_defs = (dex_class_def*)(m_dexmmap + off);
  DexClasses classes(dh->class_defs_size);
  m_classes = &classes;

  auto lwork = new class_load_work[dh->class_defs_size];
  auto wq = workqueue_foreach<class_load_work*>(class_work);
  for (uint32_t i = 0; i < dh->class_defs_size; i++) {
    lwork[i].dl = this;
    lwork[i].num = i;
    wq.add_item(&lwork[i]);
  }
  wq.run_all();
  delete[] lwork;

  gather_input_stats(stats, dh);

  return classes;
}

static void mt_balloon(DexMethod* method) { method->balloon(); }

static void balloon_all(const Scope& scope) {
  auto wq = workqueue_foreach<DexMethod*>(mt_balloon);
  walk_methods(scope, [&](DexMethod* m) {
    if (m->get_dex_code()) {
      wq.add_item(m);
    }
  });
  wq.run_all();
}

DexClasses load_classes_from_dex(const char* location, bool balloon) {
  dex_stats_t stats;
  return load_classes_from_dex(location, &stats, balloon);
}

DexClasses load_classes_from_dex(const char* location, dex_stats_t* stats, bool balloon) {
  TRACE(MAIN, 1, "Loading classes from dex from %s\n", location);
  DexLoader dl(location);
  auto classes = dl.load_dex(location, stats);
  if (balloon) {
    balloon_all(classes);
  }
  return classes;
}

void balloon_for_test(const Scope& scope) { balloon_all(scope); }
